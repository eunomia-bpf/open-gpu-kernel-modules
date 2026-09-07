#ifndef _UVM_BPF_STRUCT_OPS_H
#define _UVM_BPF_STRUCT_OPS_H

#include "uvm_ioctl.h"
#include "uvm_va_block_types.h"
#include "uvm_perf_prefetch.h"
#include "uvm_pmm_gpu.h"
#include "nv-gpu-transition-validator.h"
#include "uvm_stale_state_v1.h"

typedef struct uvm_bpf_prefetch_decision
{
    nv_gpu_prefetch_decision_t request;
} uvm_bpf_prefetch_decision_t;

enum uvm_bpf_prefetch_diagnostic_phase {
    UVM_BPF_PREFETCH_DIAG_SELECTED = 1,
    UVM_BPF_PREFETCH_DIAG_FINISHED = 2,
};

/* Driver-owned copies for a privileged, read-only tracing observer.
 * Output fields are valid only at FINISHED. */
struct uvm_bpf_prefetch_diagnostic_ctx {
    NvS64 raw_action;
    NvU64 requested_first;
    NvU64 requested_outer;
    NvU64 max_first;
    NvU64 max_outer;
    NvU64 output_first;
    NvU64 output_outer;
    NvU32 phase;
    NvU32 request_attempted;
    NvU32 request_conflict;
    NvU32 initial_region_result;
    NvU32 initial_effect;
    NvU32 native_iterations;
    NvU32 native_completed;
};

void uvm_bpf_prefetch_diagnostic(const struct uvm_bpf_prefetch_diagnostic_ctx *ctx);

typedef struct uvm_bpf_pmm_decision_ctx
{
    // All fields are driver-owned and read/write access from BPF is rejected.
    // The pointer is only valid for the current callback invocation.
    uvm_pmm_gpu_t *pmm;
    uvm_gpu_root_chunk_t *root_chunk;
    nv_gpu_pmm_snapshot_t observed;
    nv_gpu_pmm_request_t request;
} uvm_bpf_pmm_decision_ctx_t;

/* Fixed-width storage scheduling request inputs passed to the BPF hook. */
typedef struct uvm_bpf_storage_request
{
    NvU32 abi_version;
    NvU32 op;
    NvU32 request_flags;
    NvU32 input_priority;
    NvU64 request_id;
    NvU64 object_id;
    NvU64 bytes;
    NvU64 tenant_id;
    NvU64 caller_hint;
    NvU64 deadline_ns;
    NvU64 slack_ns;
    NvU64 estimated_transfer_ns;
    NvU64 recompute_ns;
    NvU32 queue_depth;
    NvU32 hbm_pressure_permille;
} uvm_bpf_storage_request_t;

/* Storage scheduling decision recorded by the BPF kfunc. All fields are
 * clamped by the kfunc before being visible to the ioctl handler. */
typedef struct uvm_bpf_storage_decision
{
    NvU32 action;
    NvU64 defer_ns;
    NvU32 priority;
    NvU32 batch_target;
} uvm_bpf_storage_decision_t;

/* Callback-local context for the storage policy hook: all request inputs
 * plus the decision recorded via bpf_gpu_storage_record(). */
typedef struct uvm_bpf_storage_decision_ctx
{
    uvm_bpf_storage_request_t request;
    uvm_bpf_storage_decision_t decision;
    NvU32 recorded;
} uvm_bpf_storage_decision_ctx_t;

/* Storage decision clamps, shared by the kfunc and the ioctl handler */
#define UVM_GPU_STORAGE_MAX_DEFER_NS     10000000ULL /* 10 ms */
#define UVM_GPU_STORAGE_MAX_PRIORITY     7U
#define UVM_GPU_STORAGE_MIN_BATCH_TARGET 1U
#define UVM_GPU_STORAGE_MAX_BATCH_TARGET 64U

/* Action codes returned by BPF hooks */
enum uvm_bpf_action {
    UVM_BPF_ACTION_DEFAULT = 0,       /* Use default kernel behavior */
    UVM_BPF_ACTION_BYPASS = 1,        /* Skip default kernel behavior */
    UVM_BPF_ACTION_ENTER_LOOP = 2,    /* Enter the tree iteration loop with BPF hooks */
};

/* Function declarations for BPF struct_ops initialization */
int uvm_bpf_struct_ops_init(void);
void uvm_bpf_struct_ops_exit(void);

/* Wrapper functions for calling BPF hooks from kernel code */
NvS64 uvm_bpf_call_gpu_page_prefetch(
    uvm_page_index_t page_index,
    uvm_perf_prefetch_bitmap_tree_t *bitmap_tree,
    uvm_va_block_region_t *max_prefetch_region,
    nv_gpu_prefetch_decision_t *decision);

NvS64 uvm_bpf_call_gpu_page_prefetch_iter(
    uvm_perf_prefetch_bitmap_tree_t *bitmap_tree,
    uvm_va_block_region_t *max_prefetch_region,
    uvm_va_block_region_t *current_region,
    unsigned int counter,
    nv_gpu_prefetch_decision_t *decision);

NvS64 uvm_bpf_call_gpu_stale_state_v1(
    uvm_stale_state_v1_decision_ctx_t *decision_ctx);

/* PMM eviction policy hook wrapper functions */
void uvm_bpf_call_gpu_block_activate(
    uvm_pmm_gpu_t *pmm,
    uvm_gpu_chunk_t *chunk);

enum nv_gpu_pmm_access_effect uvm_bpf_call_gpu_block_access(
    uvm_pmm_gpu_t *pmm,
    uvm_gpu_chunk_t *chunk);

void uvm_bpf_call_gpu_evict_prepare(
    uvm_pmm_gpu_t *pmm,
    struct list_head *va_block_used,
    struct list_head *va_block_unused);

/* GPU storage scheduling policy hook wrapper. The attached policy is always
 * invoked when registered; there is no policy selector. The context carries
 * all request inputs and receives the recorded decision. */
void uvm_bpf_call_gpu_storage_decide(
    uvm_bpf_storage_decision_ctx_t *decision);

/* Fixed-width KV reclaim candidate vector and callback-local context.
 * Mirrors gds-control/kv_reclaim_abi.h. The kernel keeps no caller state:
 * every call carries its own bounded candidate vector (max 8, fixed-width
 * scalars only) plus shared observed rates. No fd, file offset, GPU
 * pointer, arbitrary user pointer, stream, or completion crosses this
 * interface. Priority uses vLLM integer semantics: lower value is more
 * important, so the worst class is the maximum value. */
#define UVM_KV_RECLAIM_COST_SAT 0x7FFFFFFFFFFFFFFFULL /* saturating estimate cap */

typedef struct uvm_bpf_kv_reclaim_candidate
{
    NvU64 cookie;             // opaque caller token, echoed back only
    NvU64 freeable_bytes;     // KV bytes actually freeable by reclaim
    NvU64 computed_tokens;    // tokens computed so far
    NvU64 disk_backed_tokens; // contiguous disk-backed prefix tokens
    NvU64 disk_backed_bytes;  // provider's actual backed transfer bytes
    NvU32 priority;           // 0..UVM_KV_RECLAIM_MAX_PRIORITY, lower is more important
    NvU32 flags;             // UVM_KV_RECLAIM_CANDIDATE_FLAG_*
} uvm_bpf_kv_reclaim_candidate_t;

typedef struct uvm_bpf_kv_reclaim_request
{
    NvU32 abi_version;
    NvU32 n_candidates;       // 1..UVM_KV_RECLAIM_MAX_CANDIDATES
    NvU32 stock_index;        // caller's default victim index
    NvU32 pad0;               // reserved, must be 0
    NvU64 disk_read_ns_per_kib;   // shared observed rate, 0 = unknown
    NvU64 recompute_ns_per_token; // shared observed rate, 0 = unknown
} uvm_bpf_kv_reclaim_request_t;

/* KV reclaim decision recorded by the BPF kfunc. All fields are validated
 * by the kfunc (index/cookie/eligible class/route range) before being
 * visible to the ioctl handler; the estimate is clamped to the cap. */
typedef struct uvm_bpf_kv_reclaim_decision
{
    NvU32 index;              // selected candidate index
    NvU32 route;              // UVM_KV_RECLAIM_ROUTE_*
    NvU64 cookie;
    NvU64 estimated_ns;       // saturating estimated recovery cost
} uvm_bpf_kv_reclaim_decision_t;

/* Callback-local context for the KV reclaim policy hook: all inputs plus
 * the decision recorded via bpf_kv_reclaim_record(). eligible_mask is
 * precomputed by the ioctl handler and is the kernel-trusted view of
 * eligibility. */
typedef struct uvm_bpf_kv_reclaim_decision_ctx
{
    uvm_bpf_kv_reclaim_request_t request;
    uvm_bpf_kv_reclaim_candidate_t candidates[UVM_KV_RECLAIM_MAX_CANDIDATES];
    uvm_bpf_kv_reclaim_decision_t decision;
    NvU32 eligible_mask;
    NvU32 recorded;
} uvm_bpf_kv_reclaim_decision_ctx_t;

/* Worst (least important) priority class among candidates with positive
 * freeable bytes, in vLLM integer semantics: lower value is more important,
 * so the worst class is the MAXIMUM priority value. Returns 0 when none. */
static inline NvU32 uvm_kv_reclaim_worst_class(
    const uvm_bpf_kv_reclaim_decision_ctx_t *ctx, NvU32 n)
{
    NvU32 i, worst = 0, any = 0;

    for (i = 0; i < n; i++) {
        if (ctx->candidates[i].freeable_bytes == 0)
            continue;
        if (!any || ctx->candidates[i].priority > worst) {
            worst = ctx->candidates[i].priority;
            any = 1;
        }
    }
    return any ? worst : 0;
}

/* Eligible: positive freeable bytes, usable recompute rate, consistent
 * telemetry (disk_backed_tokens <= computed_tokens), and the worst
 * priority class. */
static inline NvU32 uvm_kv_reclaim_eligible(
    const uvm_bpf_kv_reclaim_decision_ctx_t *ctx, NvU32 n, NvU32 i)
{
    const uvm_bpf_kv_reclaim_candidate_t *c = &ctx->candidates[i];

    if (i >= n || c->freeable_bytes == 0)
        return 0;
    if (ctx->request.recompute_ns_per_token == 0)
        return 0;
    if (c->disk_backed_tokens > c->computed_tokens)
        return 0;
    if (c->priority != uvm_kv_reclaim_worst_class(ctx, n))
        return 0;
    return 1;
}

/* KV reclaim candidate-selection policy hook wrapper. The attached policy
 * is always invoked when registered; there is no policy selector. The
 * context carries all candidate inputs and receives the recorded decision. */
void uvm_bpf_call_gpu_kv_reclaim_choose(
    uvm_bpf_kv_reclaim_decision_ctx_t *decision);

#endif /* _UVM_BPF_STRUCT_OPS_H */
