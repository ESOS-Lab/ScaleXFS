/* xfs_log_scale.c for ScaleXFS
 *
 * Coopyright (C) 2022 OSLab, KAIST
 *
 */

#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_shared.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_extent_busy.h"
#include "xfs_trans.h"
#include "xfs_trans_priv.h"
#include "xfs_log.h"
#include "xfs_log_priv.h"
#include "xfs_trace.h"

#include "xfs_log_scale.h"

struct xlog_scale *
xlog_scale_cil_init(
	struct xlog 		*log,
	int			slen /* stride length */) 
{
	struct xlog_scale 	*scale;
	struct xlog_scale_cil 	*xscil;
	int 			idx, cpu;
        bool                    percpu = (slen>=0);

	scale = kmem_zalloc(sizeof(*scale), KM_MAYFAIL);
	if (!scale)
		return NULL;

	for (idx = 0; idx < 2; idx++) {
		xscil = &scale->xs_cil[idx];
		xscil->xsc_scale = scale;

#ifdef XFS_CTX_PCPRW_SEMA
		percpu_init_rwsem(&xscil->xsc_ctx_lock);
#else
		init_rwsem(&xscil->xsc_ctx_lock);
#endif
		spin_lock_init(&xscil->xsc_cil_lock);

		INIT_LIST_HEAD(&xscil->xsc_cil);
		INIT_LIST_HEAD(&xscil->xsc_ctx.busy_extents);

		/* initialize per-core variable */
		xscil->pcp_cil = alloc_percpu_gfp(struct list_head, GFP_KERNEL);
		xscil->pcp_ctx = alloc_percpu_gfp(xlog_scale_cilctx_t, GFP_KERNEL);

		for_each_online_cpu(cpu) {
			struct list_head    *cil;
			xlog_scale_cilctx_t *ctx;

			cil = per_cpu_ptr(xscil->pcp_cil, cpu);
			ctx = per_cpu_ptr(xscil->pcp_ctx, cpu);

			INIT_LIST_HEAD(cil);
			INIT_LIST_HEAD(&ctx->busy_extents);	
		}
		
		xscil->isempty.counter = 1;
		xscil->space_counter.counter = 0;
		xscil->being_pushed = false;
		xscil->percpu = percpu;
	}

	scale->xs_sequence = 1;
	scale->xs_unit_res = xfs_log_calc_unit_res(log->l_mp, 0);

	scale->xs_percpu = percpu;
	slen = (slen != 0) ? slen:1;
        scale->xs_step_size = ilog2(slen);

	return scale;
}

void
xlog_scale_cil_destroy(
	struct xlog 		*log)
{
	struct xlog_scale 	*scale = log->l_cilp->xc_scale;
	int 			idx;

	for (idx = 0; idx < 2; idx++) {
#ifdef XFS_CTX_PCPRW_SEMA
		percpu_free_rwsem(&scale->xs_cil[idx].xsc_ctx_lock);
#endif
		free_percpu(scale->xs_cil[idx].pcp_cil);
		free_percpu(scale->xs_cil[idx].pcp_ctx);
	}

	ASSERT(xlog_scale_allcil_empty(scale));

	kmem_free(scale);
}

void 
xlog_scale_cilctx_merge(
	struct xfs_cil 		*cilp,
	struct xfs_cil_ctx 	*newctx,
	xfs_lsn_t 		seq)
{
	struct xlog_scale_cil 	*xscil;
	xlog_scale_cilctx_t	*ctx;
	int 			idx = seq % 2;

	xscil = &cilp->xc_scale->xs_cil[idx];
	ASSERT(!xscil->percpu);

	ctx = &xscil->xsc_ctx;

	newctx->space_used	= ctx->space_used;
	newctx->nvecs 		= ctx->nvecs;
	newctx->ticket->t_curr_res = ctx->curr_res;
	newctx->ticket->t_unit_res = ctx->unit_res;
	list_splice(&ctx->busy_extents, &newctx->busy_extents);
	
	memset(ctx, 0, sizeof(*ctx));
	INIT_LIST_HEAD(&ctx->busy_extents);
}

void
xlog_scale_pcpcilctx_merge(
	struct xfs_cil 		*cilp,
	struct xfs_cil_ctx 	*newctx,
	xfs_lsn_t 		seq)
{
	struct xlog_scale_cil 	*xscil;
	int 			cpu, idx = seq % 2;

	xscil = &cilp->xc_scale->xs_cil[idx];
	ASSERT(xscil->percpu);

	for_each_online_cpu(cpu) {
		struct list_head    *cil;
		xlog_scale_cilctx_t *ctx;
		
		cil = per_cpu_ptr(xscil->pcp_cil, cpu);
		ctx = per_cpu_ptr(xscil->pcp_ctx, cpu);

		list_splice_init(cil, &xscil->xsc_cil);
		
		newctx->nvecs	   += ctx->nvecs;
		newctx->space_used += ctx->space_used;
		newctx->ticket->t_curr_res += ctx->curr_res;
		newctx->ticket->t_unit_res += ctx->unit_res;
		list_splice(&ctx->busy_extents, &newctx->busy_extents);

		memset(ctx, 0, sizeof(*ctx));
		INIT_LIST_HEAD(&ctx->busy_extents);
	}
	
	xscil->space_counter.counter = 0;
	xscil->isempty.counter = 1;
	xscil->being_pushed = false;
}

void 
xlog_scale_pcpcil_update_space_used(
	struct xlog_scale_cil	*xscil,
        int                     space_used,
        int                     len,
        int                     iclog_space,
        int                     *split_res,
	bool			*first)
{
	int step_size = xscil->xsc_scale->xs_step_size;
	int cur_space_count, new_space_count, diff_count = 0;
	int cur_space, new_space;
        int base = (1 << step_size) - 1;	

	// These counts are local value.
	cur_space_count = howmany_bitwise(space_used, base, step_size);
	new_space_count = howmany_bitwise(space_used + len, base, step_size);

	diff_count = new_space_count - cur_space_count;

	if (diff_count > 0) {
		// These counts are global value. 
		new_space_count = atomic_add_return(diff_count, &xscil->space_counter);
		cur_space_count = new_space_count - diff_count; 

		cur_space = cur_space_count << step_size;
		new_space = new_space_count << step_size; 

		if (cur_space == 0 && atomic_read(&xscil->isempty) == 1) {
			atomic_set(&xscil->isempty, 0);
			*first = true;
		}

		if ((cur_space / iclog_space) != (new_space / iclog_space)) 
			*split_res = howmany(diff_count << step_size, iclog_space);

	} else if (diff_count < 0)
		atomic_add(diff_count, &xscil->space_counter);

	return;
}

bool 
xlog_scale_cil_empty(
	struct xfs_cil 		*cilp,
	xfs_lsn_t 		seq)
{
	struct xlog_scale_cil 	*xscil;
	int 			idx = seq % 2;

	xscil = &cilp->xc_scale->xs_cil[idx];
	
	if (xscil->percpu)
		return (atomic_read(&xscil->isempty) == 1);
	else
		return list_empty(&xscil->xsc_cil);
}

bool 
xlog_scale_allcil_empty(
	struct xfs_cil 		*cilp)
{
	bool			empty0, empty1;

	empty0 = xlog_scale_cil_empty(cilp, 0);
	empty1 = xlog_scale_cil_empty(cilp, 1);

	return (empty0 && empty1);
}

bool 
xlog_scale_allcil_empty_locked(
	struct xfs_cil		*cilp)
{
	bool 			empty;

	spin_lock(&cilp->xc_push_lock);
	empty = xlog_scale_allcil_empty(cilp);
	spin_unlock(&cilp->xc_push_lock);

	return empty;
}
