/* xfs_log_scale.h for ScaleXFS
 * 
 * Copyright (C) 2022 OSLab, KAIST
 * 
 */
#ifndef __XFS_LOG_SCALE_H__
#define __XFS_LOG_SCALE_H__

#include <linux/percpu.h>
#include "libxfs/xfs_types.h"

struct xlog_scale_cil_ctx;
struct xlog_scale_cil;
struct xlog_scale;

typedef struct xlog_scale_cil_ctx {
	/* For ticket member */
	int			curr_res;
	int			unit_res;
	/* For CIL context member */
	int			nvecs;
	int			space_used;
	struct list_head	busy_extents;
} xlog_scale_cilctx_t;

#define XFS_CTX_PCPRW_SEMA
struct xlog_scale_cil {
	struct xlog_scale	*xsc_scale;
#ifdef XFS_CTX_PCPRW_SEMA
	struct percpu_rw_semaphore xsc_ctx_lock;
#else
	struct rw_semaphore	xsc_ctx_lock;
#endif
	struct spinlock		xsc_cil_lock;

	struct list_head 	xsc_cil;
	xlog_scale_cilctx_t	xsc_ctx;
	
	/* per-core in-memory logging */
	struct list_head __percpu    *pcp_cil;
	xlog_scale_cilctx_t __percpu *pcp_ctx;

	atomic_t		space_counter; 
	atomic_t		isempty; 
	bool			being_pushed;
	bool			percpu;
};

struct xlog_scale {
        /* Double committed item list */
	struct xlog_scale_cil	xs_cil[2];

	xfs_lsn_t		xs_sequence;
	int			xs_unit_res;
	int			xs_step_size;
	bool			xs_percpu;
};

struct xlog_scale *xlog_scale_cil_init(struct xlog *log, int stepsize);
void xlog_scale_cil_destroy(struct xlog *log);

void xlog_scale_cilctx_merge(struct xfs_cil *cilp,
			     struct xfs_cil_ctx *newctx,
			     xfs_lsn_t seq);
void xlog_scale_pcpcilctx_merge(struct xfs_cil *cilp,
				struct xfs_cil_ctx *newctx,
				xfs_lsn_t seq);

void xlog_scale_pcpcil_update_space_used(
			struct xlog_scale_cil *xscil,
                        int space_used, int len, int iclog_space,
                        int *split_res, bool *first);

bool xlog_scale_cil_empty(struct xfs_cil *cilp, xfs_lsn_t seq);
bool xlog_scale_allcil_empty(struct xfs_cil *cilp);
bool xlog_scale_allcil_empty_locked(struct xfs_cil *cilp);

#endif
