/**
 * Copyright (C) Mellanox Technologies Ltd. 2021.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include "config.h"
#include "tl_ucp.h"
#include "reduce.h"
#include "core/ucc_progress_queue.h"
#include "tl_ucp_sendrecv.h"
#include "utils/ucc_math.h"

#define SAVE_STATE(_phase)                                                     \
    do {                                                                       \
        task->reduce_kn.phase = _phase;                                        \
    } while (0)

ucc_status_t ucc_tl_ucp_reduce_knomial_progress(ucc_coll_task_t *coll_task)
{
    ucc_tl_ucp_task_t *task      = ucc_derived_of(coll_task, ucc_tl_ucp_task_t);
    ucc_coll_args_t   *args      = &coll_task->args;
    ucc_tl_ucp_team_t *team      = TASK_TEAM(task);
    ucc_rank_t         myrank    = team->rank;
    ucc_rank_t         team_size = team->size;
    ucc_rank_t         root      = (ucc_rank_t)args->root;
    uint32_t           radix     = task->reduce_kn.radix;
    ucc_rank_t         vrank     = (myrank - root + team_size) % team_size;
    void              *rbuf      = (myrank == root) ? args->dst.info.buffer :
                                                      task->reduce_kn.scratch;
    ucc_memory_type_t  mtype;
    ucc_datatype_t     dt;
    size_t             count, data_size;
    void              *received_vectors, *scratch_offset;
    ucc_rank_t         vpeer, peer, vroot_at_level, root_at_level, pos;
    uint32_t           i;
    ucc_status_t       status;

    if (root == myrank) {
        count = args->dst.info.count;
        data_size = count * ucc_dt_size(args->dst.info.datatype);
        mtype = args->dst.info.mem_type;
        dt = args->dst.info.datatype;
    } else {
        count = args->src.info.count;
        data_size = count * ucc_dt_size(args->src.info.datatype);
        mtype = args->src.info.mem_type;
        dt = args->src.info.datatype;
    }
    received_vectors = PTR_OFFSET(task->reduce_kn.scratch, data_size);

UCC_REDUCE_KN_PHASE_PROGRESS:
    if (UCC_INPROGRESS == ucc_tl_ucp_test(task)) {
        return task->super.super.status;
    }

    UCC_REDUCE_KN_GOTO_PHASE(task->reduce_kn.phase);

UCC_REDUCE_KN_PHASE_INIT:

    while (task->reduce_kn.dist <= task->reduce_kn.max_dist) {
        if (vrank % task->reduce_kn.dist == 0) {
            pos = (vrank / task->reduce_kn.dist) % radix;
            if (pos == 0) {
                scratch_offset = received_vectors;
                task->reduce_kn.children_per_cycle = 0;
                for (i = 1; i < radix; i++) {
                    vpeer = vrank + i * task->reduce_kn.dist;
                    if (vpeer >= team_size) {
                    	break;
                    } else {
                        task->reduce_kn.children_per_cycle += 1;
                        peer = (vpeer + root) % team_size;
                        UCPCHECK_GOTO(ucc_tl_ucp_recv_nb(scratch_offset,
                                          data_size, mtype, peer, team, task),
                                          task, out);
                        scratch_offset = PTR_OFFSET(scratch_offset, data_size);
                    }
                }
                SAVE_STATE(UCC_REDUCE_KN_PHASE_MULTI);
                goto UCC_REDUCE_KN_PHASE_PROGRESS;
UCC_REDUCE_KN_PHASE_MULTI:
                if (task->reduce_kn.children_per_cycle) {
                    status = ucc_dt_reduce_multi((task->reduce_kn.dist == 1) ?
                                     args->src.info.buffer : rbuf,
                                     received_vectors, rbuf,
                                     task->reduce_kn.children_per_cycle,
                                     count, data_size, dt, mtype, args);
                    if (ucc_unlikely(UCC_OK != status)) {
                        tl_error(UCC_TASK_LIB(task),
                                 "failed to perform dt reduction");
                        task->super.super.status = status;
                        return status;
                    }
                }
            } else {
                vroot_at_level = vrank - pos * task->reduce_kn.dist;
                root_at_level  = (vroot_at_level + root) % team_size;
                UCPCHECK_GOTO(ucc_tl_ucp_send_nb(task->reduce_kn.scratch,
                                  data_size, mtype, root_at_level, team, task),
                                  task, out);
            }
        }
        task->reduce_kn.dist *= radix;
        SAVE_STATE(UCC_REDUCE_KN_PHASE_INIT);
        goto UCC_REDUCE_KN_PHASE_PROGRESS;
    }

    ucc_assert(UCC_TL_UCP_TASK_P2P_COMPLETE(task));
    task->super.super.status = UCC_OK;
    UCC_TL_UCP_PROFILE_REQUEST_EVENT(coll_task, "ucp_reduce_kn_done", 0);
out:
    return task->super.super.status;
}

ucc_status_t ucc_tl_ucp_reduce_knomial_start(ucc_coll_task_t *coll_task)
{
    ucc_tl_ucp_task_t *task      =
        ucc_derived_of(coll_task, ucc_tl_ucp_task_t);
    ucc_coll_args_t   *args      = &coll_task->args;
    ucc_tl_ucp_team_t *team      = TASK_TEAM(task);
    uint32_t           radix     = task->reduce_kn.radix;
    ucc_rank_t         root      = (ucc_rank_t)args->root;
    ucc_rank_t         myrank    = team->rank;
    ucc_rank_t         team_size = team->size;
    ucc_rank_t         vrank     = (myrank - root + team_size) % team_size;
    int                isleaf    =
        (vrank % radix != 0 || vrank == team_size - 1);
    ucc_status_t       status;

    UCC_TL_UCP_PROFILE_REQUEST_EVENT(coll_task, "ucp_reduce_kn_start", 0);
    ucc_tl_ucp_task_reset(task);

    if (UCC_IS_INPLACE(*args) && (team->rank == root)) {
        args->src.info.buffer = args->dst.info.buffer;
    }

    if (isleaf) {
    	task->reduce_kn.scratch = args->src.info.buffer;
    }

    task->reduce_kn.dist = 1;
    task->reduce_kn.phase = UCC_REDUCE_KN_PHASE_INIT;

    status = ucc_tl_ucp_reduce_knomial_progress(&task->super);
    if (UCC_INPROGRESS == status) {
        ucc_progress_enqueue(UCC_TL_CORE_CTX(team)->pq, &task->super);
        return UCC_OK;
    }
    return ucc_task_complete(coll_task);
}

ucc_status_t ucc_tl_ucp_reduce_knomial_finalize(ucc_coll_task_t *coll_task)
{
    ucc_tl_ucp_task_t *task      =
        ucc_derived_of(coll_task, ucc_tl_ucp_task_t);

    if (task->reduce_kn.scratch_mc_header) {
        ucc_mc_free(task->reduce_kn.scratch_mc_header);
    }
    return ucc_tl_ucp_coll_finalize(coll_task);
}
