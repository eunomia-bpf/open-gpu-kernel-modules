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

#ifndef __UVM_DISK_BACKING_H__
#define __UVM_DISK_BACKING_H__

#include "uvm_common.h"
#include "uvm_forward_decl.h"
#include "uvm_lock.h"

/*
 * Durable on-disk backing for one sealed, read-only managed VA range.
 *
 * uvm_disk_backing_t is an opaque object. One instance is attached at a time
 * to a uvm_va_range_managed_t through range->disk_backing while the VA space
 * lock is held in write mode. The descriptor is reference counted (nv_kref):
 * the owning range holds one reference, and each scheduled offload work
 * descriptor holds one so that a worker always keeps a valid struct file
 * reference even if the range is torn down mid-flight.
 *
 * Locking:
 *  - backing->state_lock protects the per-page bitmaps (on_disk, pending,
 *    io_error). It is held only for short bitmap transitions and may be
 *    acquired while holding the VA block lock (the pattern used by the CPU
 *    chunk dirty-bitmap lock).
 *  - File I/O happens only with the VA block lock dropped: CPU fault/prefetch
 *    hydration drops it around kernel_read and signals a full service restart
 *    with NV_ERR_MORE_PROCESSING_REQUIRED (the UVM_VA_BLOCK_LOCK_RETRY
 *    convention), and offload writes always run from the nv_kthread_q worker
 *    without any UVM lock held.
 *
 * Data contract (v1): the range is sealed read-only for the lifetime of the
 * registration. Write faults from CPU and GPU are explicitly rejected
 * (SIGBUS / fatal fault), never silently served by mutating either memory or
 * disk. Pages that were never resident anywhere have authoritative zero
 * content.
 */

// Attach a backing to a fully-managed range and seal it read-only.
//
// Caller: holds the VA space lock in write mode; range must not already have a
// backing and must not be a zombie. Takes a reference on file, which must be a
// regular file open for reading and writing. file_offset + range_size bytes
 // of the file starting at file_offset back the range VAs [range->start,
// range->end]. The file content of never-offloaded unallocated pages is
// defined to be zeros; the driver writes explicit zeros for those pages on
// offload so the file is always authoritative after the first offload.
NV_STATUS uvm_disk_backing_register(uvm_va_range_managed_t *range,
                                    struct file *file,
                                    NvU64 file_offset);

// Detach and drop the range's reference. Safe with no backing attached. Called
// from uvm_va_range_destroy_managed().
void uvm_disk_backing_unregister(uvm_va_range_managed_t *range);

// Drop one reference. The backing (and its file reference) is freed at zero.
void uvm_disk_backing_release(uvm_disk_backing_t *backing);

// True if this VA address belongs to a sealed disk-backed range. Only valid
// while the caller holds a VA block lock or the VA space lock covering the
// address; callers on the fault path hold both as appropriate.
bool uvm_disk_backing_sealed_range(uvm_va_block_t *block);

// Reject CPU/GPU write faults into the sealed range. access_type is a
// uvm_fault_access_type_t; anything above READ is rejected.
bool uvm_disk_backing_reject_write(uvm_va_block_t *block,
                                   NvU64 address,
                                   bool is_write);

// True if the page containing address is durably on disk.
bool uvm_disk_backing_page_on_disk(uvm_disk_backing_t *backing, NvU64 address);

// True if any page in [address, address + size) is still pending offload or
// recorded I/O error for that page.
bool uvm_disk_backing_page_error(uvm_disk_backing_t *backing, NvU64 address);

// Read backing file bytes for [address, address + num_pages * PAGE_SIZE)
// into the contiguous struct page array page_base (chunk pages). Returns
// NV_OK, or NV_ERR_NO_MEMORY / NV_ERR_INVALID_STATE on I/O failure.
NV_STATUS uvm_disk_backing_read_pages(uvm_disk_backing_t *backing,
                                      NvU64 address,
                                      struct page *page_base,
                                      NvU32 num_pages);

// Write the chunk pages to the backing file. Worker-only (no UVM locks held):
// kernel_write() is allowed to sleep.
NV_STATUS uvm_disk_backing_write_pages(uvm_disk_backing_t *backing,
                                       NvU64 address,
                                       struct page *page_base,
                                       NvU32 num_pages);

// Claim [address, address + num_pages * PAGE_SIZE) for a disk->CPU hydration
// read; the caller must drop the VA block lock around the blocking read and
// then call hydrate_done() or hydrate_fail() for the same span.
bool uvm_disk_backing_claim_hydrate(uvm_disk_backing_t *backing,
                                    NvU64 address,
                                    NvU32 num_pages);

// Release a claimed hydration span after the read succeeded.
void uvm_disk_backing_hydrate_done(uvm_disk_backing_t *backing,
                                   NvU64 address,
                                   NvU32 num_pages);

// Release a claimed hydration span after the read failed: the span's error
// bits are set so the pages are never served with zero content.
void uvm_disk_backing_hydrate_fail(uvm_disk_backing_t *backing,
                                   NvU64 address,
                                   NvU32 num_pages);

// Schedule asynchronous disk offload of the CPU copies covering the block's
// region: stage any GPU-resident pages to the CPU, then queue a worker that
// writes the bytes to the file and, on success, frees the CPU chunks (and
// their CPU/GPU mappings). Called with the VA space lock held (read or write)
// and after the caller has verified a backing is attached. The block is
// locked internally via UVM_VA_BLOCK_LOCK_RETRY.
NV_STATUS uvm_disk_backing_offload_block(uvm_disk_backing_t *backing,
                                         uvm_va_block_t *block);

// Sleep until no page in [address, address + size) of this backing is pending
// offload. Interruptible.
NV_STATUS uvm_disk_backing_wait_offload(uvm_disk_backing_t *backing,
                                        NvU64 address,
                                        NvU64 size);

// Counters for the STATUS ioctl.
typedef struct
{
    NvU32 num_pages;
    NvU32 on_disk_pages;
    NvU32 pending_pages;
    NvU32 error_pages;
} uvm_disk_backing_status_t;

void uvm_disk_backing_query(uvm_disk_backing_t *backing,
                            NvU64 start,
                            NvU64 end,
                            uvm_disk_backing_status_t *out_status);

// Backing attached to the block's managed range, or NULL for HMM blocks,
// external ranges, or unbacked managed ranges. Requires uvm_va_block.h to
// have been included for the block type.
uvm_disk_backing_t *uvm_disk_backing_from_block(uvm_va_block_t *block);

// Split a backing view when its range is split: the existing range keeps the
// low half and the new range gets the high half, sharing the file reference
// and the full-range bitmaps. The caller holds the VA space lock in write
// mode and both ranges are live.
NV_STATUS uvm_disk_backing_split(uvm_va_range_managed_t *existing_range,
                                 uvm_va_range_managed_t *new_range);

#endif // __UVM_DISK_BACKING_H__
