/*
 * Copyright (C) 2016 Oracle.  All Rights Reserved.
 *
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_trans.h"
#include "xfs_trans_priv.h"
#include "xfs_refcount_item.h"
#include "xfs_alloc.h"
#include "xfs_refcount.h"

/*
 * This routine is called to allocate an "refcount update intent"
 * log item that will hold nextents worth of extents.  The
 * caller must use all nextents extents, because we are not
 * flexible about this at all.
 */
STATIC struct xfs_cui_log_item *
xfs_trans_get_cui(
	struct xfs_trans		*tp,
	uint				nextents)
{
	struct xfs_cui_log_item		*cuip;

	ASSERT(tp != NULL);
	ASSERT(nextents > 0);

	cuip = xfs_cui_init(tp->t_mountp, nextents);
	ASSERT(cuip != NULL);

	/*
	 * Get a log_item_desc to point at the new item.
	 */
	xfs_trans_add_item(tp, &cuip->cui_item);
	return cuip;
}

/* Set the phys extent flags for this reverse mapping. */
static void
xfs_trans_set_refcount_flags(
	struct xfs_phys_extent		*refc,
	enum xfs_refcount_intent_type	type)
{
	refc->pe_flags = 0;
	switch (type) {
	case XFS_REFCOUNT_INCREASE:
		refc->pe_flags |= XFS_REFCOUNT_EXTENT_INCREASE;
		break;
	case XFS_REFCOUNT_DECREASE:
		refc->pe_flags |= XFS_REFCOUNT_EXTENT_DECREASE;
		break;
	case XFS_REFCOUNT_ALLOC_COW:
		refc->pe_flags |= XFS_REFCOUNT_EXTENT_ALLOC_COW;
		break;
	case XFS_REFCOUNT_FREE_COW:
		refc->pe_flags |= XFS_REFCOUNT_EXTENT_FREE_COW;
		break;
	default:
		ASSERT(0);
	}
}

/*
 * This routine is called to indicate that the described extent is to
 * be logged as needing to have its refcount updated.  It should be
 * called once for each extent to be updated.
 */
STATIC void
xfs_trans_log_start_refcount_update(
	struct xfs_trans		*tp,
	struct xfs_cui_log_item		*cuip,
	enum xfs_refcount_intent_type	type,
	xfs_fsblock_t			startblock,
	xfs_filblks_t			blockcount)
{
	uint				next_extent;
	struct xfs_phys_extent		*refc;

	tp->t_flags |= XFS_TRANS_DIRTY;
	cuip->cui_item.li_desc->lid_flags |= XFS_LID_DIRTY;

	/*
	 * atomic_inc_return gives us the value after the increment;
	 * we want to use it as an array index so we need to subtract 1 from
	 * it.
	 */
	next_extent = atomic_inc_return(&cuip->cui_next_extent) - 1;
	ASSERT(next_extent < cuip->cui_format.cui_nextents);
	refc = &(cuip->cui_format.cui_extents[next_extent]);
	refc->pe_startblock = startblock;
	refc->pe_len = blockcount;
	xfs_trans_set_refcount_flags(refc, type);
}

/*
 * This routine is called to allocate a "refcount update done"
 * log item that will hold nextents worth of extents.  The
 * caller must use all nextents extents, because we are not
 * flexible about this at all.
 */
struct xfs_cud_log_item *
xfs_trans_get_cud(
	struct xfs_trans		*tp,
	struct xfs_cui_log_item		*cuip,
	uint				nextents)
{
	struct xfs_cud_log_item		*cudp;

	ASSERT(tp != NULL);
	ASSERT(nextents > 0);

	cudp = xfs_cud_init(tp->t_mountp, cuip, nextents);
	ASSERT(cudp != NULL);

	/*
	 * Get a log_item_desc to point at the new item.
	 */
	xfs_trans_add_item(tp, &cudp->cud_item);
	return cudp;
}

/*
 * Finish an refcount update and log it to the CUD. Note that the
 * transaction is marked dirty regardless of whether the refcount
 * update succeeds or fails to support the CUI/CUD lifecycle rules.
 */
int
xfs_trans_log_finish_refcount_update(
	struct xfs_trans		*tp,
	struct xfs_cud_log_item		*cudp,
	struct xfs_defer_ops		*dop,
	enum xfs_refcount_intent_type	type,
	xfs_fsblock_t			startblock,
	xfs_extlen_t			blockcount,
	xfs_extlen_t			*adjusted,
	struct xfs_btree_cur		**pcur)
{
	uint				next_extent;
	struct xfs_phys_extent		*refc;
	int				error;

	error = xfs_refcount_finish_one(tp, dop, type, startblock,
			blockcount, adjusted, pcur);

	/*
	 * Mark the transaction dirty, even on error. This ensures the
	 * transaction is aborted, which:
	 *
	 * 1.) releases the CUI and frees the CUD
	 * 2.) shuts down the filesystem
	 */
	tp->t_flags |= XFS_TRANS_DIRTY;
	cudp->cud_item.li_desc->lid_flags |= XFS_LID_DIRTY;

	next_extent = cudp->cud_next_extent;
	ASSERT(next_extent < cudp->cud_format.cud_nextents);
	refc = &(cudp->cud_format.cud_extents[next_extent]);
	refc->pe_startblock = startblock;
	refc->pe_len = blockcount;
	xfs_trans_set_refcount_flags(refc, type);
	cudp->cud_next_extent++;
	if (!error && *adjusted != blockcount) {
		refc->pe_len = *adjusted;
		cudp->cud_format.cud_nextents = cudp->cud_next_extent;
	}

	return error;
}

/* Sort refcount intents by AG. */
static int
xfs_refcount_update_diff_items(
	void				*priv,
	struct list_head		*a,
	struct list_head		*b)
{
	struct xfs_mount		*mp = priv;
	struct xfs_refcount_intent	*ra;
	struct xfs_refcount_intent	*rb;

	ra = container_of(a, struct xfs_refcount_intent, ri_list);
	rb = container_of(b, struct xfs_refcount_intent, ri_list);
	return  XFS_FSB_TO_AGNO(mp, ra->ri_startblock) -
		XFS_FSB_TO_AGNO(mp, rb->ri_startblock);
}

/* Get an CUI. */
STATIC void *
xfs_refcount_update_create_intent(
	struct xfs_trans		*tp,
	unsigned int			count)
{
	return xfs_trans_get_cui(tp, count);
}

/* Log refcount updates in the intent item. */
STATIC void
xfs_refcount_update_log_item(
	struct xfs_trans		*tp,
	void				*intent,
	struct list_head		*item)
{
	struct xfs_refcount_intent	*refc;

	refc = container_of(item, struct xfs_refcount_intent, ri_list);
	xfs_trans_log_start_refcount_update(tp, intent, refc->ri_type,
			refc->ri_startblock,
			refc->ri_blockcount);
}

/* Get an CUD so we can process all the deferred refcount updates. */
STATIC void *
xfs_refcount_update_create_done(
	struct xfs_trans		*tp,
	void				*intent,
	unsigned int			count)
{
	return xfs_trans_get_cud(tp, intent, count);
}

/* Process a deferred refcount update. */
STATIC int
xfs_refcount_update_finish_item(
	struct xfs_trans		*tp,
	struct xfs_defer_ops		*dop,
	struct list_head		*item,
	void				*done_item,
	void				**state)
{
	struct xfs_refcount_intent	*refc;
	xfs_extlen_t			adjusted;
	int				error;

	refc = container_of(item, struct xfs_refcount_intent, ri_list);
	error = xfs_trans_log_finish_refcount_update(tp, done_item, dop,
			refc->ri_type,
			refc->ri_startblock,
			refc->ri_blockcount,
			&adjusted,
			(struct xfs_btree_cur **)state);
	/* Did we run out of reservation?  Requeue what we didn't finish. */
	if (!error && adjusted < refc->ri_blockcount) {
		ASSERT(refc->ri_type == XFS_REFCOUNT_INCREASE ||
		       refc->ri_type == XFS_REFCOUNT_DECREASE);
		refc->ri_startblock += adjusted;
		refc->ri_blockcount -= adjusted;
		return -EAGAIN;
	}
	kmem_free(refc);
	return error;
}

/* Clean up after processing deferred refcounts. */
STATIC void
xfs_refcount_update_finish_cleanup(
	struct xfs_trans	*tp,
	void			*state,
	int			error)
{
	struct xfs_btree_cur	*rcur = state;

	xfs_refcount_finish_one_cleanup(tp, rcur, error);
}

/* Abort all pending CUIs. */
STATIC void
xfs_refcount_update_abort_intent(
	void				*intent)
{
	xfs_cui_release(intent);
}

/* Cancel a deferred refcount update. */
STATIC void
xfs_refcount_update_cancel_item(
	struct list_head		*item)
{
	struct xfs_refcount_intent	*refc;

	refc = container_of(item, struct xfs_refcount_intent, ri_list);
	kmem_free(refc);
}

static const struct xfs_defer_op_type xfs_refcount_update_defer_type = {
	.type		= XFS_DEFER_OPS_TYPE_REFCOUNT,
	.max_items	= XFS_CUI_MAX_FAST_EXTENTS,
	.diff_items	= xfs_refcount_update_diff_items,
	.create_intent	= xfs_refcount_update_create_intent,
	.abort_intent	= xfs_refcount_update_abort_intent,
	.log_item	= xfs_refcount_update_log_item,
	.create_done	= xfs_refcount_update_create_done,
	.finish_item	= xfs_refcount_update_finish_item,
	.finish_cleanup = xfs_refcount_update_finish_cleanup,
	.cancel_item	= xfs_refcount_update_cancel_item,
};

/* Register the deferred op type. */
void
xfs_refcount_update_init_defer_op(void)
{
	xfs_defer_init_op_type(&xfs_refcount_update_defer_type);
}
