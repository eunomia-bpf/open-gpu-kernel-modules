/*******************************************************************************
    Copyright (c) 2024 NVIDIA Corporation

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

        The above copyright notice and this permission notice shall be
        included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.

*******************************************************************************/

#include "uvm_common.h"
#include "uvm_disk_backing.h"
#include "uvm_global.h"
#include "uvm_pmm_sysmem.h"
#include "uvm_va_block.h"
#include "uvm_va_range.h"
#include "uvm_va_space.h"

// Shared by the backing views of one registered range and of the views
// created when that range is split. Holds the file reference and the
// full-range per-page bitmaps, indexed by absolute page within the
// originally registered range.
struct uvm_disk_backing_shared_struct
{
    nv_kref_t kref;

    // Owns one reference for the lifetime of the shared state.
    struct file *file;

    // Absolute VA and page count of the originally registered range.
    NvU64 full_start;
    NvU32 full_num_pages;

    // Byte offset of full_start within the file.
    loff_t full_file_offset;

    // Protects the bitmaps below. Short critical sections only.
    uvm_mutex_t state_lock;
    unsigned long *on_disk;    // durable copy exists in the file
    unsigned long *pending;    // claimed by an in-flight offload writer
    unsigned long *io_error;   // page whose offload I/O failed
    unsigned long *hydrating;  // page whose disk->CPU hydration read is running

    // Woken when a pending span completes or fails; used by the OFFLOAD
    // ioctl's wait and by the STATUS ioctl.
    wait_queue_head_t wq;

    // GPU promotion (see uvm_disk_backing.h): shared by every view of the
    // registered range, so it survives range splits. Written under the VA
    // space write lock, read on the fault path with the VA space lock held.
    bool gpu_promote;
};

// One backing view attached to a single managed range, or to one half of a
// split range. A view is a contiguous sub-range of the shared full range
// with its own VA/file geometry, sharing the file reference and bitmaps.
struct uvm_disk_backing_struct
{
    nv_kref_t kref;
    struct uvm_disk_backing_shared_struct *shared;

    // Absolute VA covered by this view.
    NvU64 range_start;
    NvU64 range_size;
    NvU32 num_pages;

    // Byte offset of range_start within the file.
    loff_t file_offset;

    bool sealed_read_only;
};

// One contiguously-addressed CPU chunk captured for offload. The chunk's
// nv_kref is held (nv_kref_get) for the lifetime of the descriptor so the
// chunk pages stay valid while the block/mapping state changes underneath.
typedef struct
{
    uvm_cpu_chunk_t *chunk;

    // Absolute address and page span of the whole chunk within the block.
    NvU64 abs_start;
    uvm_va_block_region_t region;

    // True if the pages were not durably on disk yet and must be written;
    // false if this group exists only to release the CPU copy of pages that
    // are already durably backed.
    bool write_needed;

    // Updated by the worker: all claimed pages written (or nothing to write).
    bool write_ok;

    // Updated by the worker: the in-memory copies were released (CPU and GPU
    // PTEs unmapped, PTE updates confirmed, CPU chunk removed from the
    // block).
    bool reclaim_ok;
} uvm_disk_backing_group_t;

static size_t backing_page_index(uvm_disk_backing_t *backing, NvU64 address)
{
    UVM_ASSERT(address >= backing->range_start);
    UVM_ASSERT(address < backing->range_start + (NvU64)backing->num_pages * PAGE_SIZE);

    // Bitmaps are indexed over the full range of the original registration.
    return (address - backing->shared->full_start) / PAGE_SIZE;
}
// One queued uvm_disk_backing_offload_block() unit. Holds references on the
// backing and the VA block, and a private tracker of the staging-copy (and
// later, unmap) operations.
typedef struct
{
    nv_kthread_q_item_t q_item;

    uvm_disk_backing_t *backing;
    uvm_va_block_t *block;
    uvm_tracker_t tracker;

    NvU32 num_groups;
    uvm_disk_backing_group_t groups[] __counted_by(num_groups);
} uvm_disk_backing_desc_t;

// True if any page of the span [(page index) first, first + num_pages) is
// claimed by an in-flight writer.
static bool backing_span_claimed(uvm_disk_backing_t *backing,
                                 size_t first,
                                 NvU32 num_pages)
{
    return find_next_bit(backing->shared->pending,
                         first + (size_t)num_pages,
                         first) < first + (size_t)num_pages;
}

// Claim [address, address + num_pages * PAGE_SIZE) for an in-flight writer.
// All pages must be free of prior claims; spans with recorded I/O errors are
// re-claimable because write_done() owns the error/valid lifecycle. Returns
// true if the whole span was atomically claimed.
static bool backing_claim_pages(uvm_disk_backing_t *backing,
                                NvU64 address,
                                NvU32 num_pages)
{
    size_t first = backing_page_index(backing, address);
    bool ok;

    uvm_mutex_lock(&backing->shared->state_lock);

    ok = !backing_span_claimed(backing, first, num_pages);
    if (ok)
        bitmap_set(backing->shared->pending, first, num_pages);

    uvm_mutex_unlock(&backing->shared->state_lock);

    return ok;
}

// Release a claimed span without completing it. Only valid on offload abort
// paths, before the worker is scheduled.
static void backing_release_claim(uvm_disk_backing_t *backing,
                                  NvU64 address,
                                  NvU32 num_pages)
{
    size_t first = backing_page_index(backing, address);

    uvm_mutex_lock(&backing->shared->state_lock);
    bitmap_clear(backing->shared->pending, first, num_pages);
    uvm_mutex_unlock(&backing->shared->state_lock);
}

// Record the I/O outcome of one claimed span. On success the span is marked
// durably on disk and any previous error bits are cleared; on failure the
// span is left not-on-disk with error bits set so that hydration never serves
// silently zero-filled data and later offload passes can retry. The pending
// bits stay set: offload completion is published only after the worker has
// released the in-memory copies (backing_region_offload_done).
static void backing_region_record_write(uvm_disk_backing_t *backing,
                                        NvU64 address,
                                        NvU32 num_pages,
                                        bool success)
{
    size_t first = backing_page_index(backing, address);

    uvm_mutex_lock(&backing->shared->state_lock);

    if (success) {
        bitmap_set(backing->shared->on_disk, first, num_pages);
        bitmap_clear(backing->shared->io_error, first, num_pages);
    }
    else {
        bitmap_set(backing->shared->io_error, first, num_pages);
    }

    uvm_mutex_unlock(&backing->shared->state_lock);
}

// Publish offload completion for one claimed span, called only after the
// worker has resolved the reclamation of the in-memory copies: the span
// leaves the pending state so QUERY no longer reports it in flight, and the
// waiters of uvm_disk_backing_wait_offload() may proceed.
static void backing_region_offload_done(uvm_disk_backing_t *backing,
                                        NvU64 address,
                                        NvU32 num_pages)
{
    size_t first = backing_page_index(backing, address);

    uvm_mutex_lock(&backing->shared->state_lock);
    bitmap_clear(backing->shared->pending, first, num_pages);
    uvm_mutex_unlock(&backing->shared->state_lock);

    wake_up_all(&backing->shared->wq);
}

// Record a failed release of one claimed span whose file write succeeded:
// the in-memory copies are kept resident so no mapping is left pointing at
// released memory, and the span is moved to the error state so QUERY reports
// that the offload pass did not complete. The on_disk bits are cleared while
// the error bits are set, keeping the two states exclusive; a later offload
// pass rewrites the span and retries the release.
static void backing_region_reclaim_fail(uvm_disk_backing_t *backing,
                                        NvU64 address,
                                        NvU32 num_pages)
{
    size_t first = backing_page_index(backing, address);

    uvm_mutex_lock(&backing->shared->state_lock);
    bitmap_clear(backing->shared->on_disk, first, num_pages);
    bitmap_set(backing->shared->io_error, first, num_pages);
    uvm_mutex_unlock(&backing->shared->state_lock);
}

static void backing_shared_free(nv_kref_t *nv_kref);

// Revoke the write permission on every existing CPU and GPU mapping of the
// range so the sealed read-only contract is enforced on mappings that predate
// the registration, not only on new faults.
//
// The caller holds the VA space lock in write mode.
static NV_STATUS backing_seal_range(uvm_va_range_managed_t *range)
{
    uvm_va_space_t *va_space = range->va_range.va_space;
    struct mm_struct *mm;
    uvm_va_block_context_t *block_context;
    uvm_va_block_t *block;
    uvm_tracker_t seal_tracker = UVM_TRACKER_INIT();
    NV_STATUS status = NV_OK;

    mm = uvm_va_space_mm_retain_lock(va_space);
    if (!mm)
        return NV_ERR_INVALID_STATE;

    block_context = uvm_va_block_context_alloc(mm);
    if (!block_context) {
        uvm_va_space_mm_release_unlock(va_space, mm);
        return NV_ERR_NO_MEMORY;
    }

    for_each_va_block_in_va_range(range, block) {
        uvm_va_block_region_t region = uvm_va_block_region_from_block(block);
        uvm_tracker_t local_tracker = UVM_TRACKER_INIT();

        // The CPU revocation and all GPU revocations run under one block-lock
        // hold so PTE operations on the same GPU stay serialized.
        status = UVM_VA_BLOCK_LOCK_RETRY(block,
                                         NULL,
                                         ({
                                             NV_STATUS st = uvm_va_block_revoke_prot(block,
                                                                                     block_context,
                                                                                     UVM_ID_CPU,
                                                                                     region,
                                                                                     NULL,
                                                                                     UVM_PROT_READ_WRITE,
                                                                                     &local_tracker);
                                             uvm_processor_id_t pid;

                                             if (st == NV_OK) {
                                                 for_each_id_in_mask(pid, &block->mapped) {
                                                     if (UVM_ID_IS_CPU(pid))
                                                         continue;

                                                     st = uvm_va_block_revoke_prot(block,
                                                                                    block_context,
                                                                                    pid,
                                                                                    region,
                                                                                    NULL,
                                                                                    UVM_PROT_READ_WRITE,
                                                                                    &local_tracker);
                                                     if (st != NV_OK)
                                                         break;
                                                 }
                                             }

                                             // The pushed PTE update work must be
                                             // tracked by the block before the lock
                                             // is dropped.
                                             if (st == NV_OK)
                                                 st = uvm_tracker_add_tracker_safe(&block->tracker,
                                                                                   &local_tracker);
                                             st;
                                         }));
        if (status != NV_OK)
            break;

        // Also track the work on the seal tracker so it can be waited on
        // after all blocks are done.
        status = uvm_tracker_add_tracker_safe(&seal_tracker, &local_tracker);
        if (status != NV_OK)
            break;
    }

    if (status == NV_OK)
        uvm_tracker_wait(&seal_tracker);

    uvm_va_block_context_free(block_context);
    uvm_va_space_mm_release_unlock(va_space, mm);

    return status;
}

NV_STATUS uvm_disk_backing_register(uvm_va_range_managed_t *range,
                                    struct file *file,
                                    NvU64 file_offset)
{
    struct uvm_disk_backing_shared_struct *shared;
    uvm_disk_backing_t *backing;
    NvU64 range_size;
    NvU32 num_pages;
    NV_STATUS status = NV_OK;

    UVM_ASSERT(range);
    UVM_ASSERT(file);

    uvm_assert_rwsem_locked_write(&range->va_range.va_space->lock);

    if (uvm_va_range_is_managed_zombie(&range->va_range))
        return NV_ERR_INVALID_STATE;

    if (range->disk_backing)
        return NV_ERR_STATE_IN_USE;

    range_size = uvm_va_range_size(&range->va_range);
    num_pages = range_size / PAGE_SIZE;

    shared = kzalloc(sizeof(*shared), GFP_KERNEL);
    if (!shared)
        return NV_ERR_NO_MEMORY;

    shared->on_disk = bitmap_zalloc(num_pages, GFP_KERNEL);
    shared->pending = bitmap_zalloc(num_pages, GFP_KERNEL);
    shared->io_error = bitmap_zalloc(num_pages, GFP_KERNEL);
    shared->hydrating = bitmap_zalloc(num_pages, GFP_KERNEL);
    if (!shared->on_disk || !shared->pending ||
        !shared->io_error || !shared->hydrating) {
        // kref is not initialized yet at this point: free everything
        // explicitly, the file reference was not taken.
        kvfree(shared->on_disk);
        kvfree(shared->pending);
        kvfree(shared->io_error);
        kvfree(shared->hydrating);
        kfree(shared);
        status = NV_ERR_NO_MEMORY;
        return status;
    }

    nv_kref_init(&shared->kref);
    shared->file = file;
    shared->full_start = range->va_range.node.start;
    shared->full_num_pages = num_pages;
    shared->full_file_offset = file_offset;
    uvm_mutex_init(&shared->state_lock, UVM_LOCK_ORDER_LEAF);
    init_waitqueue_head(&shared->wq);

    // Own the file reference before any path can drop the shared kref, so
    // the shared-free callback always has a matching fput.
    get_file(file);

    backing = kzalloc(sizeof(*backing), GFP_KERNEL);
    if (!backing) {
        status = NV_ERR_NO_MEMORY;
        goto free_shared;
    }

    nv_kref_init(&backing->kref);
    backing->shared = shared;
    backing->range_start = range->va_range.node.start;
    backing->range_size = range_size;
    backing->num_pages = num_pages;
    backing->file_offset = file_offset;
    backing->sealed_read_only = true;

    range->disk_backing = backing;

    // Seal the range read-only: revoke existing CPU/GPU write mappings so the
    // sealed contract is enforced on mappings that predate the registration,
    // not just on new faults. Roll back the whole registration on failure.
    status = backing_seal_range(range);
    if (status != NV_OK) {
        range->disk_backing = NULL;
        uvm_disk_backing_release(backing);
        return status;
    }

    return NV_OK;

free_shared:
    // shared->kref is 1 at this point, so the put drops it straight to zero.
    nv_kref_put(&shared->kref, backing_shared_free);
    return status;
}

static void backing_shared_free(nv_kref_t *nv_kref)
{
    struct uvm_disk_backing_shared_struct *shared =
        container_of(nv_kref, struct uvm_disk_backing_shared_struct, kref);

    fput(shared->file);
    kvfree(shared->on_disk);
    kvfree(shared->pending);
    kvfree(shared->io_error);
    kvfree(shared->hydrating);
    kfree(shared);
}

static void backing_free(nv_kref_t *nv_kref)
{
    uvm_disk_backing_t *backing = container_of(nv_kref, uvm_disk_backing_t, kref);

    nv_kref_put(&backing->shared->kref, backing_shared_free);
    kfree(backing);
}

// Split a backing view when the owning range is split: after the split the
// existing range keeps the low half [existing->range_start, new_end] and the
// new range gets the high half [new_end + 1, end]. Both views share the file
// reference and the full-range bitmaps, so offload/hydration state survives
// the split at every address. A failed split tears down both ranges and
// each range's unregister releases its own view, so it is safe to leave the
// existing view untouched on failure.
//
// The caller holds the VA space lock in write mode, so no other thread can
// observe the ranges in a half-split state.
NV_STATUS uvm_disk_backing_split(uvm_va_range_managed_t *existing_range,
                                 uvm_va_range_managed_t *new_range)
{
    uvm_disk_backing_t *existing = existing_range->disk_backing;
    uvm_disk_backing_t *new_view;
    NvU32 left_pages;
    NvU32 right_pages;

    UVM_ASSERT(existing);
    UVM_ASSERT(new_range->va_range.node.end ==
               existing->range_start + (NvU64)existing->num_pages * PAGE_SIZE - 1);

    left_pages = uvm_va_range_size(&existing_range->va_range) / PAGE_SIZE;
    right_pages = uvm_va_range_size(&new_range->va_range) / PAGE_SIZE;
    UVM_ASSERT(left_pages + right_pages == existing->num_pages);
    UVM_ASSERT(left_pages > 0 && right_pages > 0);

    new_view = kzalloc(sizeof(*new_view), GFP_KERNEL);
    if (!new_view)
        return NV_ERR_NO_MEMORY;

    nv_kref_init(&new_view->kref);
    new_view->shared = existing->shared;
    nv_kref_get(&new_view->shared->kref);
    new_view->range_start = existing->range_start + (NvU64)left_pages * PAGE_SIZE;
    new_view->range_size = (NvU64)right_pages * PAGE_SIZE;
    new_view->num_pages = right_pages;
    new_view->file_offset = existing->file_offset + (loff_t)(left_pages * PAGE_SIZE);
    new_view->sealed_read_only = true;
    new_range->disk_backing = new_view;

    // Re-scope the existing view down to the low half.
    existing->range_size = (NvU64)left_pages * PAGE_SIZE;
    existing->num_pages = left_pages;

    return NV_OK;
}

// Detach from the range and drop the range's reference. Called from
// uvm_va_range_destroy_managed() with the VA space lock held in write mode.
// In-flight workers hold their own references, so the backing (and the file
// reference) survives until they complete.
void uvm_disk_backing_unregister(uvm_va_range_managed_t *range)
{
    uvm_disk_backing_t *backing = range->disk_backing;

    if (!backing)
        return;

    uvm_assert_rwsem_locked_write(&range->va_range.va_space->lock);

    range->disk_backing = NULL;
    nv_kref_put(&backing->kref, backing_free);
}

void uvm_disk_backing_release(uvm_disk_backing_t *backing)
{
    nv_kref_put(&backing->kref, backing_free);
}

static void backing_get(uvm_disk_backing_t *backing)
{
    nv_kref_get(&backing->kref);
}

// True if every page of the span is durably on disk.
static bool backing_span_on_disk(uvm_disk_backing_t *backing,
                                 NvU64 address,
                                 NvU32 num_pages)
{
    size_t first = backing_page_index(backing, address);
    bool ret;

    uvm_mutex_lock(&backing->shared->state_lock);
    ret = find_next_zero_bit(backing->shared->on_disk,
                              first + (size_t)num_pages,
                              first) == first + (size_t)num_pages;
    uvm_mutex_unlock(&backing->shared->state_lock);

    return ret;
}

typedef enum
{
    BACKING_PAGE_ZERO = 0,  // authoritative zero, never backed by the file
    BACKING_PAGE_ON_DISK,   // durable file copy exists
    BACKING_PAGE_PENDING,   // claimed by an in-flight offload worker
    BACKING_PAGE_ERROR,     // offload pass failed (I/O error, or the
                            // in-memory copies could not be released); not
                            // zero, not on disk
    BACKING_PAGE_HYDRATING, // disk->CPU hydration read in progress
} backing_page_state_t;

static backing_page_state_t backing_page_state(uvm_disk_backing_t *backing,
                                               NvU64 address)
{
    bool bits[4];

    uvm_mutex_lock(&backing->shared->state_lock);

    bits[0] = test_bit(backing_page_index(backing, address), backing->shared->on_disk);
    bits[1] = test_bit(backing_page_index(backing, address), backing->shared->pending);
    bits[2] = test_bit(backing_page_index(backing, address), backing->shared->io_error);
    bits[3] = test_bit(backing_page_index(backing, address), backing->shared->hydrating);

    uvm_mutex_unlock(&backing->shared->state_lock);

    UVM_ASSERT(!(bits[0] && bits[2]) && "on-disk and error are exclusive");

    if (bits[3])
        return BACKING_PAGE_HYDRATING;
    if (bits[1])
        return BACKING_PAGE_PENDING;
    if (bits[0])
        return BACKING_PAGE_ON_DISK;
    if (bits[2])
        return BACKING_PAGE_ERROR;

    return BACKING_PAGE_ZERO;
}

// Claim the pages of [address, address + num_pages * PAGE_SIZE) for a
// disk->CPU hydration read. The span must currently carry on-disk state for
// the pages to be hydrated (the caller checked) and must have no pending
// offload nor concurrent hydration claim. Returns true if claimed; the
// caller must drop the VA block lock around the blocking file read while the
// claim is held.
bool uvm_disk_backing_claim_hydrate(uvm_disk_backing_t *backing,
                                    NvU64 address,
                                    NvU32 num_pages)
{
    size_t first = backing_page_index(backing, address);
    bool ok;

    uvm_mutex_lock(&backing->shared->state_lock);

    ok = find_next_bit(backing->shared->pending,
                        first + (size_t)num_pages,
                        first) == first + (size_t)num_pages &&
          find_next_bit(backing->shared->hydrating,
                        first + (size_t)num_pages,
                        first) == first + (size_t)num_pages;
    if (ok)
        bitmap_set(backing->shared->hydrating, first, num_pages);

    uvm_mutex_unlock(&backing->shared->state_lock);

    return ok;
}

void uvm_disk_backing_hydrate_done(uvm_disk_backing_t *backing,
                                   NvU64 address,
                                   NvU32 num_pages)
{
    size_t first = backing_page_index(backing, address);

    uvm_mutex_lock(&backing->shared->state_lock);
    bitmap_clear(backing->shared->hydrating, first, num_pages);
    uvm_mutex_unlock(&backing->shared->state_lock);

    wake_up_all(&backing->shared->wq);
}

// Record a failed hydration read: the span is left not-on-disk with error
// bits set so later faults fail explicitly instead of serving zeros.
void uvm_disk_backing_hydrate_fail(uvm_disk_backing_t *backing,
                                   NvU64 address,
                                   NvU32 num_pages)
{
    size_t first = backing_page_index(backing, address);

    uvm_mutex_lock(&backing->shared->state_lock);
    bitmap_clear(backing->shared->hydrating, first, num_pages);
    bitmap_set(backing->shared->io_error, first, num_pages);
    uvm_mutex_unlock(&backing->shared->state_lock);

    wake_up_all(&backing->shared->wq);
}

static void backing_offload_worker_entry(void *args);

// Stage any GPU-resident pages of the block to the CPU, capture the block's
// CPU chunks (claiming their spans and taking one reference on each chunk),
// and schedule the offload worker.
//
// Locking: the VA block lock must be held, with the VA space lock held in
// at least read mode as required by the staging copy.
static NV_STATUS backing_offload_queue_locked(uvm_disk_backing_t *backing,
                                              uvm_va_block_t *block,
                                              uvm_va_block_context_t *block_context)
{
    uvm_va_block_region_t block_region = uvm_va_block_region_from_block(block);
    uvm_page_mask_t union_gpu;
    uvm_page_mask_t stage_mask;
    uvm_disk_backing_desc_t *desc;
    uvm_page_index_t page_index;
    uvm_processor_id_t id;
    NvU32 num_chunks = 0;
    NvU32 captured = 0;
    NvU32 i;
    NV_STATUS status = NV_OK;

    // Stage any GPU-resident pages to the CPU so a single CPU copy covers
    // the whole block.
    uvm_page_mask_zero(&union_gpu);

    for_each_gpu_id_in_mask(id, &block->resident) {
        uvm_page_mask_or(&union_gpu,
                         &union_gpu,
                         uvm_va_block_resident_mask_get(block, id, NUMA_NO_NODE));
    }

    if (!uvm_page_mask_empty(&union_gpu)) {
        uvm_page_mask_andnot(&stage_mask,
                             &union_gpu,
                             uvm_va_block_resident_mask_get(block, UVM_ID_CPU, NUMA_NO_NODE));

        if (!uvm_page_mask_empty(&stage_mask)) {
            status = uvm_va_block_make_resident(block,
                                                NULL,
                                                block_context,
                                                UVM_ID_CPU,
                                                block_region,
                                                &stage_mask,
                                                NULL,
                                                UVM_MAKE_RESIDENT_CAUSE_EVICTION);
            if (status != NV_OK)
                return status;
        }
    }

    // Count the CPU chunks to capture so the descriptor can be sized.
    for (page_index = block_region.first; page_index < block_region.outer; ) {
        uvm_cpu_chunk_t *chunk = uvm_va_block_get_cpu_chunk_for_page(block, page_index);

        if (chunk) {
            ++num_chunks;
            page_index += uvm_cpu_chunk_num_pages(chunk);
        }
        else {
            ++page_index;
        }
    }

    if (num_chunks == 0)
        return NV_OK;

    desc = kvzalloc(sizeof(*desc) + num_chunks * sizeof(desc->groups[0]), GFP_KERNEL);
    if (!desc)
        return NV_ERR_NO_MEMORY;

    desc->backing = backing;
    desc->block = block;
    uvm_tracker_init(&desc->tracker);

    page_index = block_region.first;
    while (page_index < block_region.outer) {
        uvm_cpu_chunk_t *chunk = uvm_va_block_get_cpu_chunk_for_page(block, page_index);
        uvm_disk_backing_group_t *group;
        NvU32 num_pages;

        if (!chunk) {
            ++page_index;
            continue;
        }

        num_pages = uvm_cpu_chunk_num_pages(chunk);
        group = &desc->groups[captured];

        if (!backing_claim_pages(backing,
                                 uvm_va_block_cpu_page_address(block, page_index),
                                 num_pages)) {
            // A concurrent offload is already moving this span to disk.
            status = NV_ERR_STATE_IN_USE;
            goto out_release;
        }

        group->chunk = chunk;
        nv_kref_get(&chunk->refcount);
        group->abs_start = uvm_va_block_cpu_page_address(block, page_index);
        group->region = uvm_va_block_region(page_index, page_index + num_pages);
        group->write_needed = !backing_span_on_disk(backing, group->abs_start, num_pages);
        group->write_ok = false;

        ++captured;
        page_index += num_pages;
    }

    desc->num_groups = captured;

    status = uvm_tracker_add_tracker_safe(&desc->tracker, &block->tracker);
    if (status != NV_OK)
        goto out_release;

    uvm_va_block_retain(block);
    backing_get(backing);

    nv_kthread_q_item_init(&desc->q_item, backing_offload_worker_entry, desc);
    if (!nv_kthread_q_schedule_q_item(&g_uvm_global.global_q, &desc->q_item)) {
        // The item was not consumed; release the references taken above and
        // fall through to the capture cleanup.
        status = NV_ERR_BUSY_RETRY;
        uvm_va_block_release_no_destroy(block);
        uvm_disk_backing_release(backing);
        goto out_release;
    }

    return NV_OK;

out_release:
    for (i = 0; i < captured; ++i) {
        uvm_disk_backing_group_t *group = &desc->groups[i];

        backing_release_claim(backing, group->abs_start,
                              uvm_cpu_chunk_num_pages(group->chunk));
        uvm_cpu_chunk_free(group->chunk);
    }

    kvfree(desc);

    return status;
}

// Offload one VA block: stage every GPU-resident page of the block to the
// CPU (making it usable as a durable staging copy), capture the CPU chunks
// (one reference each), and queue a worker for the physical transfer and
// final copy release. Runs with the VA space lock held (read or write); the
// block lock is taken internally with the UVM_VA_BLOCK_LOCK_RETRY convention.
NV_STATUS uvm_disk_backing_offload_block(uvm_disk_backing_t *backing,
                                         uvm_va_block_t *block)
{
    uvm_va_space_t *va_space = uvm_va_block_get_va_space(block);
    struct mm_struct *mm;
    uvm_va_block_context_t *block_context;
    NV_STATUS status;

    UVM_ASSERT(block);

    if (uvm_disk_backing_from_block(block) != backing)
        return NV_ERR_INVALID_STATE;

    mm = uvm_va_space_mm_retain(va_space);
    if (!mm)
        return NV_ERR_NO_MEMORY;

    block_context = uvm_va_block_context_alloc(mm);
    if (!block_context) {
        uvm_va_space_mm_release(va_space);
        return NV_ERR_NO_MEMORY;
    }

    // NV_ERR_MORE_PROCESSING_REQUIRED from the staging copy is retried by
    // the macro; on each retry the whole queueing step re-runs from a clean
    // state under the re-locked block.
    status = UVM_VA_BLOCK_LOCK_RETRY(block,
                                     NULL,
                                     backing_offload_queue_locked(backing,
                                                                  block,
                                                                  block_context));

    uvm_va_block_context_free(block_context);
    uvm_va_space_mm_release(va_space);

    return status;
}

// Offload worker: write the captured chunk pages to the backing file, then
// release the CPU copies of the groups that made it to disk. The q_item is
// embedded in the descriptor, which is freed at the very end.
static void backing_offload_worker_entry(void *args)
{
    uvm_disk_backing_desc_t *desc = args;
    uvm_disk_backing_t *backing = desc->backing;
    uvm_va_block_t *block = desc->block;
    uvm_va_space_t *va_space;
    struct mm_struct *mm = NULL;
    uvm_va_block_context_t *block_context = NULL;
    NvU32 i;

    // Wait for the staging copies and any other tracked block work to
    // complete so the captured chunk pages carry their final content. The
    // captured chunk references keep the pages valid from here on.
    uvm_tracker_wait(&desc->tracker);

    // File writes run with no UVM locks held; kernel_write() may sleep.
    for (i = 0; i < desc->num_groups; ++i) {
        uvm_disk_backing_group_t *group = &desc->groups[i];

        if (!group->write_needed) {
            group->write_ok = true;
            // The span was already durably backed at capture time; record the
            // outcome, the pending bits are released after the in-memory
            // copies are reclaimed below.
            backing_region_record_write(backing,
                                        group->abs_start,
                                        uvm_cpu_chunk_num_pages(group->chunk),
                                        true);
            continue;
        }

        group->write_ok =
            (uvm_disk_backing_write_pages(backing,
                                          group->abs_start,
                                          group->chunk->page,
                                          uvm_cpu_chunk_num_pages(group->chunk)) == NV_OK);

        backing_region_record_write(backing,
                                    group->abs_start,
                                    uvm_cpu_chunk_num_pages(group->chunk),
                                    group->write_ok);
    }

    // Release the CPU copies of the groups that made it to disk: unmap the
    // CPU PTEs and any GPU PTEs mapped to those CPU pages, wait for the PTE
    // updates, then drop the chunks from the block. Later CPU or GPU faults
    // on these addresses re-hydrate from the file at the same VA.
    va_space = uvm_va_block_get_va_space_maybe_dead(block);
    if (va_space) {
        mm = uvm_va_space_mm_retain(va_space);
        if (mm)
            block_context = uvm_va_block_context_alloc(mm);
    }

    for (i = 0; i < desc->num_groups; ++i) {
        uvm_disk_backing_group_t *group = &desc->groups[i];
        uvm_tracker_t unmap_tracker = UVM_TRACKER_INIT();
        NV_STATUS unmap_status = NV_OK;

        if (!group->write_ok || uvm_va_block_is_dead(block) || !block_context)
            continue;

        // The CPU unmap and all GPU unmaps run under one block-lock hold so
        // PTE operations on the same GPU stay serialized.
        unmap_status = UVM_VA_BLOCK_LOCK_RETRY(block,
                                               NULL,
                                               ({
                                                   NV_STATUS st = uvm_va_block_unmap(block,
                                                                                     block_context,
                                                                                     UVM_ID_CPU,
                                                                                     group->region,
                                                                                     NULL,
                                                                                     &unmap_tracker);
                                                   uvm_processor_id_t pid;

                                                   if (st == NV_OK) {
                                                       for_each_id_in_mask(pid, &block->mapped) {
                                                           if (UVM_ID_IS_CPU(pid))
                                                               continue;

                                                           st = uvm_va_block_unmap(block,
                                                                                    block_context,
                                                                                    pid,
                                                                                    group->region,
                                                                                    NULL,
                                                                                    &unmap_tracker);
                                                           if (st != NV_OK)
                                                               break;
                                                       }
                                                   }
                                                   st;
                                               }));

        if (unmap_status != NV_OK) {
            // The pages may still be mapped: keep the CPU copy resident so no
            // mapping is left pointing at freed memory. The failure is
            // published with the pending bits below.
            continue;
        }

        // Let the PTE updates complete before the pages may be released; a
        // failing PTE update means the mappings were not all removed, so the
        // copy must be kept.
        if (uvm_tracker_wait(&unmap_tracker) != NV_OK)
            continue;

        uvm_mutex_lock(&block->lock);

        // Re-check: the chunk may have been moved by a concurrent CPU->GPU
        // migration (our kref kept the pages valid); only remove it if the
        // block still points at it. If the region still carries a CPU chunk
        // afterwards, the release did not happen and is published as a
        // failure below.
        if (uvm_va_block_get_cpu_chunk_for_page(block, group->region.first) == group->chunk) {
            uvm_va_block_remove_cpu_chunks(block, group->region);
            group->reclaim_ok = true;
        }

        uvm_mutex_unlock(&block->lock);
    }

    // Publish offload completion only after the reclamation outcome is
    // resolved, so QUERY pending=0 means the in-memory copies have been
    // released, or that the failure to release them is visible in the error
    // counters: a successful write whose release failed moves the span to
    // the error state (the retained copy stays resident and authoritative,
    // and a later offload pass rewrites the span and retries the release).
    for (i = 0; i < desc->num_groups; ++i) {
        uvm_disk_backing_group_t *group = &desc->groups[i];

        if (group->write_ok && !group->reclaim_ok)
            backing_region_reclaim_fail(backing,
                                        group->abs_start,
                                        uvm_cpu_chunk_num_pages(group->chunk));

        backing_region_offload_done(backing,
                                    group->abs_start,
                                    uvm_cpu_chunk_num_pages(group->chunk));
    }

    if (block_context)
        uvm_va_block_context_free(block_context);
    if (mm)
        uvm_va_space_mm_release(va_space);

    for (i = 0; i < desc->num_groups; ++i)
        uvm_cpu_chunk_free(desc->groups[i].chunk);

    uvm_va_block_release_no_destroy(block);
    uvm_disk_backing_release(backing);
    kvfree(desc);
}

NV_STATUS uvm_disk_backing_wait_offload(uvm_disk_backing_t *backing,
                                        NvU64 address,
                                        NvU64 size)
{
    size_t first = backing_page_index(backing, address);
    size_t outer = first + (size / PAGE_SIZE);
    int ret;

    UVM_ASSERT(backing);
    UVM_ASSERT(size % PAGE_SIZE == 0);
    UVM_ASSERT(address + size <= backing->range_start +
               (NvU64)backing->num_pages * PAGE_SIZE);

    for (;;) {
        uvm_mutex_lock(&backing->shared->state_lock);
        ret = (find_next_bit(backing->shared->pending, outer, first) == outer);
        uvm_mutex_unlock(&backing->shared->state_lock);

        if (ret)
            return NV_OK;

        ret = wait_event_interruptible(backing->shared->wq,
                                       find_next_bit(backing->shared->pending, outer, first) == outer);
        if (ret == -ERESTARTSYS)
            return NV_ERR_BUSY_RETRY;
    }
}

uvm_disk_backing_t *uvm_disk_backing_from_block(uvm_va_block_t *block)
{
    if (uvm_va_block_is_hmm(block) || block->managed_range == NULL)
        return NULL;

    return block->managed_range->disk_backing;
}

bool uvm_disk_backing_sealed_range(uvm_va_block_t *block)
{
    uvm_disk_backing_t *backing = uvm_disk_backing_from_block(block);

    return backing && backing->sealed_read_only;
}

NV_STATUS uvm_disk_backing_set_gpu_promote(uvm_va_range_managed_t *range,
                                           bool enable)
{
    uvm_disk_backing_t *backing = range->disk_backing;

    uvm_assert_rwsem_locked_write(&range->va_range.va_space->lock);

    if (!backing)
        return NV_ERR_INVALID_STATE;

    backing->shared->gpu_promote = enable;

    return NV_OK;
}

bool uvm_disk_backing_gpu_promote(uvm_disk_backing_t *backing)
{
    return backing && backing->shared->gpu_promote;
}

bool uvm_disk_backing_reject_write(uvm_va_block_t *block, NvU64 address, bool is_write)
{
    uvm_disk_backing_t *backing = uvm_disk_backing_from_block(block);

    if (!backing || !backing->sealed_read_only)
        return false;

    UVM_ASSERT(address >= block->start);
    UVM_ASSERT(address <= block->end);

    return is_write;
}

bool uvm_disk_backing_page_on_disk(uvm_disk_backing_t *backing, NvU64 address)
{
    bool ret;

    uvm_mutex_lock(&backing->shared->state_lock);
    ret = test_bit(backing_page_index(backing, address), backing->shared->on_disk);
    uvm_mutex_unlock(&backing->shared->state_lock);

    return ret;
}

bool uvm_disk_backing_page_error(uvm_disk_backing_t *backing, NvU64 address)
{
    bool ret;

    uvm_mutex_lock(&backing->shared->state_lock);
    ret = test_bit(backing_page_index(backing, address), backing->shared->io_error);
    uvm_mutex_unlock(&backing->shared->state_lock);

    return ret;
}

void uvm_disk_backing_query(uvm_disk_backing_t *backing,
                            NvU64 start,
                            NvU64 end,
                            uvm_disk_backing_status_t *out_status)
{
    size_t first, last;

    memset(out_status, 0, sizeof(*out_status));
    out_status->num_pages = backing->num_pages;

    first = backing_page_index(backing, start);
    last = backing_page_index(backing, end);

    uvm_mutex_lock(&backing->shared->state_lock);
    for (; first <= last; ++first) {
        if (test_bit(first, backing->shared->on_disk))
            ++out_status->on_disk_pages;
        if (test_bit(first, backing->shared->pending))
            ++out_status->pending_pages;
        if (test_bit(first, backing->shared->io_error))
            ++out_status->error_pages;
    }
    uvm_mutex_unlock(&backing->shared->state_lock);
}

// Copy up to num_pages 4K pages of data between the backing file and the
// physically-contiguous pages starting at page_base (a CPU chunk). Consecutive
// pages are coalesced into single kernel_read()/kernel_write() calls; the
// direct map is contiguous for contiguous PFNs, so page_address() runs are
// valid. Non-contiguous runs fall back to per-page transfers.
static NV_STATUS backing_rw_pages(uvm_disk_backing_t *backing,
                                  NvU64 address,
                                  struct page *page_base,
                                  NvU32 num_pages,
                                  bool is_read)
{
    NvU32 i = 0;

    UVM_ASSERT(num_pages > 0);

    while (i < num_pages) {
        NvU32 run;
        loff_t pos = backing->file_offset +
                     (address - backing->range_start) / PAGE_SIZE * PAGE_SIZE +
                     (NvU64)i * PAGE_SIZE;
        NV_STATUS status = NV_OK;

        // Extend the run while PFNs are consecutive.
        for (run = 1; i + run < num_pages; ++run) {
            if (page_to_pfn(&page_base[i + run]) !=
                page_to_pfn(&page_base[i + run - 1]) + 1)
                break;
        }

        while (run > 0) {
            size_t count = (size_t)run * PAGE_SIZE;
            char *buf = page_address(page_base + i);
            ssize_t ret;

            if (is_read)
                ret = kernel_read(backing->shared->file, buf, count, &pos);
            else
                ret = kernel_write(backing->shared->file, buf, count, &pos);

            if (ret < 0 || (size_t)ret != count) {
                UVM_ERR_PRINT("disk backing %s pages [0x%llx, 0x%llx): %zd/%zd\n",
                              is_read ? "read" : "write",
                              address + (NvU64)i * PAGE_SIZE,
                              address + (NvU64)(i + run) * PAGE_SIZE,
                              ret < 0 ? (ssize_t)0 : ret,
                              count);
                status = NV_ERR_INVALID_STATE;
                break;
            }

            i += run;
            run = 0;
        }

        if (status != NV_OK)
            return status;
    }

    return NV_OK;
}

    NV_STATUS uvm_disk_backing_read_pages(uvm_disk_backing_t *backing,
                                      NvU64 address,
                                      struct page *page_base,
                                      NvU32 num_pages)
{
    return backing_rw_pages(backing, address, page_base, num_pages, true);
}

NV_STATUS uvm_disk_backing_write_pages(uvm_disk_backing_t *backing,
                                       NvU64 address,
                                       struct page *page_base,
                                       NvU32 num_pages)
{
    return backing_rw_pages(backing, address, page_base, num_pages, false);
}
