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
#include "xfs_sb.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_btree.h"
#include "xfs_bmap.h"
#include "xfs_refcount_btree.h"
#include "xfs_alloc.h"
#include "xfs_error.h"
#include "xfs_trace.h"
#include "xfs_cksum.h"
#include "xfs_trans.h"
#include "xfs_bit.h"
#include "xfs_refcount.h"
#include "xfs_rmap.h"
#include "xfs_scrub.h"

/* Allowable refcount adjustment amounts. */
enum xfs_refc_adjust_op {
	XFS_REFCOUNT_ADJUST_INCREASE	= 1,
	XFS_REFCOUNT_ADJUST_DECREASE	= -1,
	XFS_REFCOUNT_ADJUST_COW_ALLOC	= 0,
	XFS_REFCOUNT_ADJUST_COW_FREE	= -1,
};

STATIC int __xfs_refcount_cow_alloc(struct xfs_btree_cur *rcur,
		xfs_agblock_t agbno, xfs_extlen_t aglen,
		struct xfs_defer_ops *dfops);
STATIC int __xfs_refcount_cow_free(struct xfs_btree_cur *rcur,
		xfs_agblock_t agbno, xfs_extlen_t aglen,
		struct xfs_defer_ops *dfops);

/*
 * Look up the first record less than or equal to [bno, len] in the btree
 * given by cur.
 */
int
xfs_refcount_lookup_le(
	struct xfs_btree_cur	*cur,
	xfs_agblock_t		bno,
	int			*stat)
{
	trace_xfs_refcount_lookup(cur->bc_mp, cur->bc_private.a.agno, bno,
			XFS_LOOKUP_LE);
	cur->bc_rec.rc.rc_startblock = bno;
	cur->bc_rec.rc.rc_blockcount = 0;
	return xfs_btree_lookup(cur, XFS_LOOKUP_LE, stat);
}

/*
 * Look up the first record greater than or equal to [bno, len] in the btree
 * given by cur.
 */
int
xfs_refcount_lookup_ge(
	struct xfs_btree_cur	*cur,
	xfs_agblock_t		bno,
	int			*stat)
{
	trace_xfs_refcount_lookup(cur->bc_mp, cur->bc_private.a.agno, bno,
			XFS_LOOKUP_GE);
	cur->bc_rec.rc.rc_startblock = bno;
	cur->bc_rec.rc.rc_blockcount = 0;
	return xfs_btree_lookup(cur, XFS_LOOKUP_GE, stat);
}

/*
 * Get the data from the pointed-to record.
 */
int
xfs_refcount_get_rec(
	struct xfs_btree_cur		*cur,
	struct xfs_refcount_irec	*irec,
	int				*stat)
{
	union xfs_btree_rec	*rec;
	int			error;

	error = xfs_btree_get_rec(cur, &rec, stat);
	if (!error && *stat == 1) {
		irec->rc_startblock = be32_to_cpu(rec->refc.rc_startblock);
		irec->rc_blockcount = be32_to_cpu(rec->refc.rc_blockcount);
		irec->rc_refcount = be32_to_cpu(rec->refc.rc_refcount);
		trace_xfs_refcount_get(cur->bc_mp, cur->bc_private.a.agno,
				irec);
	}
	return error;
}

/*
 * Update the record referred to by cur to the value given
 * by [bno, len, refcount].
 * This either works (return 0) or gets an EFSCORRUPTED error.
 */
STATIC int
xfs_refcount_update(
	struct xfs_btree_cur		*cur,
	struct xfs_refcount_irec	*irec)
{
	union xfs_btree_rec	rec;
	int			error;

	trace_xfs_refcount_update(cur->bc_mp, cur->bc_private.a.agno, irec);
	rec.refc.rc_startblock = cpu_to_be32(irec->rc_startblock);
	rec.refc.rc_blockcount = cpu_to_be32(irec->rc_blockcount);
	rec.refc.rc_refcount = cpu_to_be32(irec->rc_refcount);
	error = xfs_btree_update(cur, &rec);
	if (error)
		trace_xfs_refcount_update_error(cur->bc_mp,
				cur->bc_private.a.agno, error, _RET_IP_);
	return error;
}

/*
 * Insert the record referred to by cur to the value given
 * by [bno, len, refcount].
 * This either works (return 0) or gets an EFSCORRUPTED error.
 */
STATIC int
xfs_refcount_insert(
	struct xfs_btree_cur		*cur,
	struct xfs_refcount_irec	*irec,
	int				*i)
{
	int				error;

	trace_xfs_refcount_insert(cur->bc_mp, cur->bc_private.a.agno, irec);
	cur->bc_rec.rc.rc_startblock = irec->rc_startblock;
	cur->bc_rec.rc.rc_blockcount = irec->rc_blockcount;
	cur->bc_rec.rc.rc_refcount = irec->rc_refcount;
	error = xfs_btree_insert(cur, i);
	XFS_WANT_CORRUPTED_GOTO(cur->bc_mp, *i == 1, out_error);
out_error:
	if (error)
		trace_xfs_refcount_insert_error(cur->bc_mp,
				cur->bc_private.a.agno, error, _RET_IP_);
	return error;
}

/*
 * Remove the record referred to by cur, then set the pointer to the spot
 * where the record could be re-inserted, in case we want to increment or
 * decrement the cursor.
 * This either works (return 0) or gets an EFSCORRUPTED error.
 */
STATIC int
xfs_refcount_delete(
	struct xfs_btree_cur	*cur,
	int			*i)
{
	struct xfs_refcount_irec	irec;
	int			found_rec;
	int			error;

	error = xfs_refcount_get_rec(cur, &irec, &found_rec);
	if (error)
		goto out_error;
	XFS_WANT_CORRUPTED_GOTO(cur->bc_mp, found_rec == 1, out_error);
	trace_xfs_refcount_delete(cur->bc_mp, cur->bc_private.a.agno, &irec);
	error = xfs_btree_delete(cur, i);
	XFS_WANT_CORRUPTED_GOTO(cur->bc_mp, *i == 1, out_error);
	if (error)
		goto out_error;
	error = xfs_refcount_lookup_ge(cur, irec.rc_startblock, &found_rec);
out_error:
	if (error)
		trace_xfs_refcount_delete_error(cur->bc_mp,
				cur->bc_private.a.agno, error, _RET_IP_);
	return error;
}

/*
 * Adjusting the Reference Count
 *
 * As stated elsewhere, the reference count btree (refcbt) stores
 * >1 reference counts for extents of physical blocks.  In this
 * operation, we're either raising or lowering the reference count of
 * some subrange stored in the tree:
 *
 *      <------ adjustment range ------>
 * ----+   +---+-----+ +--+--------+---------
 *  2  |   | 3 |  4  | |17|   55   |   10
 * ----+   +---+-----+ +--+--------+---------
 * X axis is physical blocks number;
 * reference counts are the numbers inside the rectangles
 *
 * The first thing we need to do is to ensure that there are no
 * refcount extents crossing either boundary of the range to be
 * adjusted.  For any extent that does cross a boundary, split it into
 * two extents so that we can increment the refcount of one of the
 * pieces later:
 *
 *      <------ adjustment range ------>
 * ----+   +---+-----+ +--+--------+----+----
 *  2  |   | 3 |  2  | |17|   55   | 10 | 10
 * ----+   +---+-----+ +--+--------+----+----
 *
 * For this next step, let's assume that all the physical blocks in
 * the adjustment range are mapped to a file and are therefore in use
 * at least once.  Therefore, we can infer that any gap in the
 * refcount tree within the adjustment range represents a physical
 * extent with refcount == 1:
 *
 *      <------ adjustment range ------>
 * ----+---+---+-----+-+--+--------+----+----
 *  2  |"1"| 3 |  2  |1|17|   55   | 10 | 10
 * ----+---+---+-----+-+--+--------+----+----
 *      ^
 *
 * For each extent that falls within the interval range, figure out
 * which extent is to the left or the right of that extent.  Now we
 * have a left, current, and right extent.  If the new reference count
 * of the center extent enables us to merge left, center, and right
 * into one record covering all three, do so.  If the center extent is
 * at the left end of the range, abuts the left extent, and its new
 * reference count matches the left extent's record, then merge them.
 * If the center extent is at the right end of the range, abuts the
 * right extent, and the reference counts match, merge those.  In the
 * example, we can left merge (assuming an increment operation):
 *
 *      <------ adjustment range ------>
 * --------+---+-----+-+--+--------+----+----
 *    2    | 3 |  2  |1|17|   55   | 10 | 10
 * --------+---+-----+-+--+--------+----+----
 *          ^
 *
 * For all other extents within the range, adjust the reference count
 * or delete it if the refcount falls below 2.  If we were
 * incrementing, the end result looks like this:
 *
 *      <------ adjustment range ------>
 * --------+---+-----+-+--+--------+----+----
 *    2    | 4 |  3  |2|18|   56   | 11 | 10
 * --------+---+-----+-+--+--------+----+----
 *
 * The result of a decrement operation looks as such:
 *
 *      <------ adjustment range ------>
 * ----+   +---+       +--+--------+----+----
 *  2  |   | 2 |       |16|   54   |  9 | 10
 * ----+   +---+       +--+--------+----+----
 *      DDDD    111111DD
 *
 * The blocks marked "D" are freed; the blocks marked "1" are only
 * referenced once and therefore the record is removed from the
 * refcount btree.
 */

#define RCNEXT(rc)	((rc).rc_startblock + (rc).rc_blockcount)
/*
 * Split a refcount extent that crosses agbno.
 */
STATIC int
xfs_refcount_split_extent(
	struct xfs_btree_cur		*cur,
	xfs_agblock_t			agbno,
	bool				*shape_changed)
{
	struct xfs_refcount_irec	rcext, tmp;
	int				found_rec;
	int				error;

	*shape_changed = false;
	error = xfs_refcount_lookup_le(cur, agbno, &found_rec);
	if (error)
		goto out_error;
	if (!found_rec)
		return 0;

	error = xfs_refcount_get_rec(cur, &rcext, &found_rec);
	if (error)
		goto out_error;
	XFS_WANT_CORRUPTED_GOTO(cur->bc_mp, found_rec == 1, out_error);
	if (rcext.rc_startblock == agbno || RCNEXT(rcext) <= agbno)
		return 0;

	*shape_changed = true;
	trace_xfs_refcount_split_extent(cur->bc_mp, cur->bc_private.a.agno,
			&rcext, agbno);

	/* Establish the right extent. */
	tmp = rcext;
	tmp.rc_startblock = agbno;
	tmp.rc_blockcount -= (agbno - rcext.rc_startblock);
	error = xfs_refcount_update(cur, &tmp);
	if (error)
		goto out_error;

	/* Insert the left extent. */
	tmp = rcext;
	tmp.rc_blockcount = agbno - rcext.rc_startblock;
	error = xfs_refcount_insert(cur, &tmp, &found_rec);
	if (error)
		goto out_error;
	XFS_WANT_CORRUPTED_GOTO(cur->bc_mp, found_rec == 1, out_error);
	return error;

out_error:
	trace_xfs_refcount_split_extent_error(cur->bc_mp,
			cur->bc_private.a.agno, error, _RET_IP_);
	return error;
}

/*
 * Merge the left, center, and right extents.
 */
STATIC int
xfs_refcount_merge_center_extent(
	struct xfs_btree_cur		*cur,
	struct xfs_refcount_irec	*left,
	struct xfs_refcount_irec	*center,
	unsigned long long		extlen,
	xfs_agblock_t			*agbno,
	xfs_extlen_t			*aglen)
{
	int				error;
	int				found_rec;

	error = xfs_refcount_lookup_ge(cur, center->rc_startblock,
			&found_rec);
	if (error)
		goto out_error;
	XFS_WANT_CORRUPTED_GOTO(cur->bc_mp, found_rec == 1, out_error);

	error = xfs_refcount_delete(cur, &found_rec);
	if (error)
		goto out_error;
	XFS_WANT_CORRUPTED_GOTO(cur->bc_mp, found_rec == 1, out_error);

	if (center->rc_refcount > 1) {
		error = xfs_refcount_delete(cur, &found_rec);
		if (error)
			goto out_error;
		XFS_WANT_CORRUPTED_GOTO(cur->bc_mp, found_rec == 1,
				out_error);
	}

	error = xfs_refcount_lookup_le(cur, left->rc_startblock,
			&found_rec);
	if (error)
		goto out_error;
	XFS_WANT_CORRUPTED_GOTO(cur->bc_mp, found_rec == 1, out_error);

	left->rc_blockcount = extlen;
	error = xfs_refcount_update(cur, left);
	if (error)
		goto out_error;

	*aglen = 0;
	return error;

out_error:
	trace_xfs_refcount_merge_center_extents_error(cur->bc_mp,
			cur->bc_private.a.agno, error, _RET_IP_);
	return error;
}

/*
 * Merge with the left extent.
 */
STATIC int
xfs_refcount_merge_left_extent(
	struct xfs_btree_cur		*cur,
	struct xfs_refcount_irec	*left,
	struct xfs_refcount_irec	*cleft,
	xfs_agblock_t			*agbno,
	xfs_extlen_t			*aglen)
{
	int				error;
	int				found_rec;

	if (cleft->rc_refcount > 1) {
		error = xfs_refcount_lookup_le(cur, cleft->rc_startblock,
				&found_rec);
		if (error)
			goto out_error;
		XFS_WANT_CORRUPTED_GOTO(cur->bc_mp, found_rec == 1,
				out_error);

		error = xfs_refcount_delete(cur, &found_rec);
		if (error)
			goto out_error;
		XFS_WANT_CORRUPTED_GOTO(cur->bc_mp, found_rec == 1,
				out_error);
	}

	error = xfs_refcount_lookup_le(cur, left->rc_startblock,
			&found_rec);
	if (error)
		goto out_error;
	XFS_WANT_CORRUPTED_GOTO(cur->bc_mp, found_rec == 1, out_error);

	left->rc_blockcount += cleft->rc_blockcount;
	error = xfs_refcount_update(cur, left);
	if (error)
		goto out_error;

	*agbno += cleft->rc_blockcount;
	*aglen -= cleft->rc_blockcount;
	return error;

out_error:
	trace_xfs_refcount_merge_left_extent_error(cur->bc_mp,
			cur->bc_private.a.agno, error, _RET_IP_);
	return error;
}

/*
 * Merge with the right extent.
 */
STATIC int
xfs_refcount_merge_right_extent(
	struct xfs_btree_cur		*cur,
	struct xfs_refcount_irec	*right,
	struct xfs_refcount_irec	*cright,
	xfs_agblock_t			*agbno,
	xfs_extlen_t			*aglen)
{
	int				error;
	int				found_rec;

	if (cright->rc_refcount > 1) {
		error = xfs_refcount_lookup_le(cur, cright->rc_startblock,
			&found_rec);
		if (error)
			goto out_error;
		XFS_WANT_CORRUPTED_GOTO(cur->bc_mp, found_rec == 1,
				out_error);

		error = xfs_refcount_delete(cur, &found_rec);
		if (error)
			goto out_error;
		XFS_WANT_CORRUPTED_GOTO(cur->bc_mp, found_rec == 1,
				out_error);
	}

	error = xfs_refcount_lookup_le(cur, right->rc_startblock,
			&found_rec);
	if (error)
		goto out_error;
	XFS_WANT_CORRUPTED_GOTO(cur->bc_mp, found_rec == 1, out_error);

	right->rc_startblock -= cright->rc_blockcount;
	right->rc_blockcount += cright->rc_blockcount;
	error = xfs_refcount_update(cur, right);
	if (error)
		goto out_error;

	*aglen -= cright->rc_blockcount;
	return error;

out_error:
	trace_xfs_refcount_merge_right_extent_error(cur->bc_mp,
			cur->bc_private.a.agno, error, _RET_IP_);
	return error;
}

#define XFS_FIND_RCEXT_SHARED	1
#define XFS_FIND_RCEXT_COW	2
/*
 * Find the left extent and the one after it (cleft).  This function assumes
 * that we've already split any extent crossing agbno.
 */
STATIC int
xfs_refcount_find_left_extents(
	struct xfs_btree_cur		*cur,
	struct xfs_refcount_irec	*left,
	struct xfs_refcount_irec	*cleft,
	xfs_agblock_t			agbno,
	xfs_extlen_t			aglen,
	int				flags)
{
	struct xfs_refcount_irec	tmp;
	int				error;
	int				found_rec;

	left->rc_blockcount = cleft->rc_blockcount = 0;
	error = xfs_refcount_lookup_le(cur, agbno - 1, &found_rec);
	if (error)
		goto out_error;
	if (!found_rec)
		return 0;

	error = xfs_refcount_get_rec(cur, &tmp, &found_rec);
	if (error)
		goto out_error;
	XFS_WANT_CORRUPTED_GOTO(cur->bc_mp, found_rec == 1, out_error);

	if (RCNEXT(tmp) != agbno)
		return 0;
	if ((flags & XFS_FIND_RCEXT_SHARED) && tmp.rc_refcount < 2)
		return 0;
	if ((flags & XFS_FIND_RCEXT_COW) && tmp.rc_refcount > 1)
		return 0;
	/* We have a left extent; retrieve (or invent) the next right one */
	*left = tmp;

	error = xfs_btree_increment(cur, 0, &found_rec);
	if (error)
		goto out_error;
	if (found_rec) {
		error = xfs_refcount_get_rec(cur, &tmp, &found_rec);
		if (error)
			goto out_error;
		XFS_WANT_CORRUPTED_GOTO(cur->bc_mp, found_rec == 1,
				out_error);

		/* if tmp starts at the end of our range, just use that */
		if (tmp.rc_startblock == agbno)
			*cleft = tmp;
		else {
			/*
			 * There's a gap in the refcntbt at the start of the
			 * range we're interested in (refcount == 1) so
			 * create the implied extent and pass it back.
			 */
			cleft->rc_startblock = agbno;
			cleft->rc_blockcount = min(aglen,
					tmp.rc_startblock - agbno);
			cleft->rc_refcount = 1;
		}
	} else {
		/*
		 * No extents, so pretend that there's one covering the whole
		 * range.
		 */
		cleft->rc_startblock = agbno;
		cleft->rc_blockcount = aglen;
		cleft->rc_refcount = 1;
	}
	trace_xfs_refcount_find_left_extent(cur->bc_mp, cur->bc_private.a.agno,
			left, cleft, agbno);
	return error;

out_error:
	trace_xfs_refcount_find_left_extent_error(cur->bc_mp,
			cur->bc_private.a.agno, error, _RET_IP_);
	return error;
}

/*
 * Find the right extent and the one before it (cright).  This function
 * assumes that we've already split any extents crossing agbno + aglen.
 */
STATIC int
xfs_refcount_find_right_extents(
	struct xfs_btree_cur		*cur,
	struct xfs_refcount_irec	*right,
	struct xfs_refcount_irec	*cright,
	xfs_agblock_t			agbno,
	xfs_extlen_t			aglen,
	int				flags)
{
	struct xfs_refcount_irec	tmp;
	int				error;
	int				found_rec;

	right->rc_blockcount = cright->rc_blockcount = 0;
	error = xfs_refcount_lookup_ge(cur, agbno + aglen, &found_rec);
	if (error)
		goto out_error;
	if (!found_rec)
		return 0;

	error = xfs_refcount_get_rec(cur, &tmp, &found_rec);
	if (error)
		goto out_error;
	XFS_WANT_CORRUPTED_GOTO(cur->bc_mp, found_rec == 1, out_error);

	if (tmp.rc_startblock != agbno + aglen)
		return 0;
	if ((flags & XFS_FIND_RCEXT_SHARED) && tmp.rc_refcount < 2)
		return 0;
	if ((flags & XFS_FIND_RCEXT_COW) && tmp.rc_refcount > 1)
		return 0;
	/* We have a right extent; retrieve (or invent) the next left one */
	*right = tmp;

	error = xfs_btree_decrement(cur, 0, &found_rec);
	if (error)
		goto out_error;
	if (found_rec) {
		error = xfs_refcount_get_rec(cur, &tmp, &found_rec);
		if (error)
			goto out_error;
		XFS_WANT_CORRUPTED_GOTO(cur->bc_mp, found_rec == 1,
				out_error);

		/* if tmp ends at the end of our range, just use that */
		if (RCNEXT(tmp) == agbno + aglen)
			*cright = tmp;
		else {
			/*
			 * There's a gap in the refcntbt at the end of the
			 * range we're interested in (refcount == 1) so
			 * create the implied extent and pass it back.
			 */
			cright->rc_startblock = max(agbno, RCNEXT(tmp));
			cright->rc_blockcount = right->rc_startblock -
					cright->rc_startblock;
			cright->rc_refcount = 1;
		}
	} else {
		/*
		 * No extents, so pretend that there's one covering the whole
		 * range.
		 */
		cright->rc_startblock = agbno;
		cright->rc_blockcount = aglen;
		cright->rc_refcount = 1;
	}
	trace_xfs_refcount_find_right_extent(cur->bc_mp, cur->bc_private.a.agno,
			cright, right, agbno + aglen);
	return error;

out_error:
	trace_xfs_refcount_find_right_extent_error(cur->bc_mp,
			cur->bc_private.a.agno, error, _RET_IP_);
	return error;
}
#undef RCNEXT

/*
 * Try to merge with any extents on the boundaries of the adjustment range.
 */
STATIC int
xfs_refcount_merge_extents(
	struct xfs_btree_cur	*cur,
	xfs_agblock_t		*agbno,
	xfs_extlen_t		*aglen,
	enum xfs_refc_adjust_op adjust,
	int			flags,
	bool			*shape_changed)
{
	struct xfs_refcount_irec	left = {0}, cleft = {0};
	struct xfs_refcount_irec	cright = {0}, right = {0};
	int				error;
	unsigned long long		ulen;
	bool				cequal;

	*shape_changed = false;
	/*
	 * Find the extent just below agbno [left], just above agbno [cleft],
	 * just below (agbno + aglen) [cright], and just above (agbno + aglen)
	 * [right].
	 */
	error = xfs_refcount_find_left_extents(cur, &left, &cleft, *agbno,
			*aglen, flags);
	if (error)
		return error;
	error = xfs_refcount_find_right_extents(cur, &right, &cright, *agbno,
			*aglen, flags);
	if (error)
		return error;

	/* No left or right extent to merge; exit. */
	if (left.rc_blockcount == 0 && right.rc_blockcount == 0)
		return 0;

	*shape_changed = true;
	cequal = (cleft.rc_startblock == cright.rc_startblock) &&
		 (cleft.rc_blockcount == cright.rc_blockcount);

	/* Try to merge left, cleft, and right.  cleft must == cright. */
	ulen = (unsigned long long)left.rc_blockcount + cleft.rc_blockcount +
			right.rc_blockcount;
	if (left.rc_blockcount != 0 && right.rc_blockcount != 0 &&
	    cleft.rc_blockcount != 0 && cright.rc_blockcount != 0 &&
	    cequal &&
	    left.rc_refcount == cleft.rc_refcount + adjust &&
	    right.rc_refcount == cleft.rc_refcount + adjust &&
	    ulen < MAXREFCEXTLEN) {
		trace_xfs_refcount_merge_center_extents(cur->bc_mp,
				cur->bc_private.a.agno, &left, &cleft, &right);
		return xfs_refcount_merge_center_extent(cur, &left, &cleft,
				ulen, agbno, aglen);
	}

	/* Try to merge left and cleft. */
	ulen = (unsigned long long)left.rc_blockcount + cleft.rc_blockcount;
	if (left.rc_blockcount != 0 && cleft.rc_blockcount != 0 &&
	    left.rc_refcount == cleft.rc_refcount + adjust &&
	    ulen < MAXREFCEXTLEN) {
		trace_xfs_refcount_merge_left_extent(cur->bc_mp,
				cur->bc_private.a.agno, &left, &cleft);
		error = xfs_refcount_merge_left_extent(cur, &left, &cleft,
				agbno, aglen);
		if (error)
			return error;

		/*
		 * If we just merged left + cleft and cleft == cright,
		 * we no longer have a cright to merge with right.  We're done.
		 */
		if (cequal)
			return 0;
	}

	/* Try to merge cright and right. */
	ulen = (unsigned long long)right.rc_blockcount + cright.rc_blockcount;
	if (right.rc_blockcount != 0 && cright.rc_blockcount != 0 &&
	    right.rc_refcount == cright.rc_refcount + adjust &&
	    ulen < MAXREFCEXTLEN) {
		trace_xfs_refcount_merge_right_extent(cur->bc_mp,
				cur->bc_private.a.agno, &cright, &right);
		return xfs_refcount_merge_right_extent(cur, &right, &cright,
				agbno, aglen);
	}

	return error;
}

/*
 * While we're adjusting the refcounts records of an extent, we have
 * to keep an eye on the number of extents we're dirtying -- run too
 * many in a single transaction and we'll exceed the transaction's
 * reservation and crash the fs.  Each record adds 12 bytes to the
 * log (plus any key updates) so we'll conservatively assume 24 bytes
 * per record.  We must also leave space for btree splits on both ends
 * of the range and space for the CUD and a new CUI.
 *
 * XXX: This is a pretty hand-wavy estimate.  The penalty for guessing
 * true incorrectly is a shutdown FS; the penalty for guessing false
 * incorrectly is more transaction rolls than might be necessary.
 * Be conservative here.
 */
static bool
xfs_refcount_still_have_space(
	struct xfs_btree_cur		*cur)
{
	unsigned long			overhead;

	overhead = cur->bc_private.a.priv.refc.shape_changes *
			xfs_allocfree_log_count(cur->bc_mp, 1);
	overhead *= cur->bc_mp->m_sb.sb_blocksize;

	/*
	 * Only allow 2 refcount extent updates per transaction if the
	 * refcount continue update "error" has been injected.
	 */
	if (cur->bc_private.a.priv.refc.nr_ops > 2 &&
	    XFS_TEST_ERROR(false, cur->bc_mp,
			XFS_ERRTAG_REFCOUNT_CONTINUE_UPDATE,
			XFS_RANDOM_REFCOUNT_CONTINUE_UPDATE))
		return false;

	if (cur->bc_private.a.priv.refc.nr_ops == 0)
		return true;
	else if (overhead > cur->bc_tp->t_log_res)
		return false;
	return  cur->bc_tp->t_log_res - overhead >
		cur->bc_private.a.priv.refc.nr_ops * 32;
}

/*
 * Adjust the refcounts of middle extents.  At this point we should have
 * split extents that crossed the adjustment range; merged with adjacent
 * extents; and updated agbno/aglen to reflect the merges.  Therefore,
 * all we have to do is update the extents inside [agbno, agbno + aglen].
 */
STATIC int
xfs_refcount_adjust_extents(
	struct xfs_btree_cur	*cur,
	xfs_agblock_t		agbno,
	xfs_extlen_t		aglen,
	xfs_extlen_t		*adjusted,
	enum xfs_refc_adjust_op	adj,
	struct xfs_defer_ops	*dfops,
	struct xfs_owner_info	*oinfo)
{
	struct xfs_refcount_irec	ext, tmp;
	int				error;
	int				found_rec, found_tmp;
	xfs_fsblock_t			fsbno;

	/* Merging did all the work already. */
	if (aglen == 0)
		return 0;

	error = xfs_refcount_lookup_ge(cur, agbno, &found_rec);
	if (error)
		goto out_error;

	while (aglen > 0 && xfs_refcount_still_have_space(cur)) {
		error = xfs_refcount_get_rec(cur, &ext, &found_rec);
		if (error)
			goto out_error;
		if (!found_rec) {
			ext.rc_startblock = cur->bc_mp->m_sb.sb_agblocks;
			ext.rc_blockcount = 0;
			ext.rc_refcount = 0;
		}

		/*
		 * Deal with a hole in the refcount tree; if a file maps to
		 * these blocks and there's no refcountbt recourd, pretend that
		 * there is one with refcount == 1.
		 */
		if (ext.rc_startblock != agbno) {
			tmp.rc_startblock = agbno;
			tmp.rc_blockcount = min(aglen,
					ext.rc_startblock - agbno);
			tmp.rc_refcount = 1 + adj;
			trace_xfs_refcount_modify_extent(cur->bc_mp,
					cur->bc_private.a.agno, &tmp);

			/*
			 * Either cover the hole (increment) or
			 * delete the range (decrement).
			 */
			if (tmp.rc_refcount) {
				error = xfs_refcount_insert(cur, &tmp,
						&found_tmp);
				if (error)
					goto out_error;
				XFS_WANT_CORRUPTED_GOTO(cur->bc_mp,
						found_tmp == 1, out_error);
				cur->bc_private.a.priv.refc.nr_ops++;
			} else {
				fsbno = XFS_AGB_TO_FSB(cur->bc_mp,
						cur->bc_private.a.agno,
						tmp.rc_startblock);
				xfs_bmap_add_free(cur->bc_mp, dfops, fsbno,
						tmp.rc_blockcount, oinfo);
			}

			(*adjusted) += tmp.rc_blockcount;
			agbno += tmp.rc_blockcount;
			aglen -= tmp.rc_blockcount;

			error = xfs_refcount_lookup_ge(cur, agbno,
					&found_rec);
			if (error)
				goto out_error;
		}

		/* Stop if there's nothing left to modify */
		if (aglen == 0 || !xfs_refcount_still_have_space(cur))
			break;

		/*
		 * Adjust the reference count and either update the tree
		 * (incr) or free the blocks (decr).
		 */
		if (ext.rc_refcount == MAXREFCOUNT)
			goto skip;
		ext.rc_refcount += adj;
		trace_xfs_refcount_modify_extent(cur->bc_mp,
				cur->bc_private.a.agno, &ext);
		if (ext.rc_refcount > 1) {
			error = xfs_refcount_update(cur, &ext);
			if (error)
				goto out_error;
			cur->bc_private.a.priv.refc.nr_ops++;
		} else if (ext.rc_refcount == 1) {
			error = xfs_refcount_delete(cur, &found_rec);
			if (error)
				goto out_error;
			XFS_WANT_CORRUPTED_GOTO(cur->bc_mp,
					found_rec == 1, out_error);
			cur->bc_private.a.priv.refc.nr_ops++;
			goto advloop;
		} else {
			fsbno = XFS_AGB_TO_FSB(cur->bc_mp,
					cur->bc_private.a.agno,
					ext.rc_startblock);
			xfs_bmap_add_free(cur->bc_mp, dfops, fsbno,
					ext.rc_blockcount, oinfo);
		}

skip:
		error = xfs_btree_increment(cur, 0, &found_rec);
		if (error)
			goto out_error;

advloop:
		(*adjusted) += ext.rc_blockcount;
		agbno += ext.rc_blockcount;
		aglen -= ext.rc_blockcount;
	}

	return error;
out_error:
	trace_xfs_refcount_modify_extent_error(cur->bc_mp,
			cur->bc_private.a.agno, error, _RET_IP_);
	return error;
}

/* Adjust the reference count of a range of AG blocks. */
STATIC int
xfs_refcount_adjust(
	struct xfs_btree_cur	*cur,
	xfs_agblock_t		agbno,
	xfs_extlen_t		aglen,
	xfs_extlen_t		*adjusted,
	enum xfs_refc_adjust_op	adj,
	struct xfs_defer_ops	*dfops,
	struct xfs_owner_info	*oinfo)
{
	xfs_extlen_t		orig_aglen;
	bool			shape_changed;
	int			shape_changes = 0;
	int			error;

	*adjusted = 0;
	switch (adj) {
	case XFS_REFCOUNT_ADJUST_INCREASE:
		trace_xfs_refcount_increase(cur->bc_mp, cur->bc_private.a.agno,
				agbno, aglen);
		break;
	case XFS_REFCOUNT_ADJUST_DECREASE:
		trace_xfs_refcount_decrease(cur->bc_mp, cur->bc_private.a.agno,
				agbno, aglen);
		break;
	default:
		ASSERT(0);
	}

	/*
	 * Ensure that no rcextents cross the boundary of the adjustment range.
	 */
	error = xfs_refcount_split_extent(cur, agbno, &shape_changed);
	if (error)
		goto out_error;
	if (shape_changed)
		shape_changes++;

	error = xfs_refcount_split_extent(cur, agbno + aglen, &shape_changed);
	if (error)
		goto out_error;
	if (shape_changed)
		shape_changes++;

	/*
	 * Try to merge with the left or right extents of the range.
	 */
	orig_aglen = aglen;
	error = xfs_refcount_merge_extents(cur, &agbno, &aglen, adj,
			XFS_FIND_RCEXT_SHARED, &shape_changed);
	if (error)
		goto out_error;
	if (shape_changed)
		shape_changes++;
	(*adjusted) += orig_aglen - aglen;
	if (shape_changes)
		cur->bc_private.a.priv.refc.shape_changes++;

	/* Now that we've taken care of the ends, adjust the middle extents */
	error = xfs_refcount_adjust_extents(cur, agbno, aglen, adjusted, adj,
			dfops, oinfo);
	if (error)
		goto out_error;

	return 0;

out_error:
	trace_xfs_refcount_adjust_error(cur->bc_mp, cur->bc_private.a.agno,
			error, _RET_IP_);
	return error;
}

/* Clean up after calling xfs_refcount_finish_one. */
void
xfs_refcount_finish_one_cleanup(
	struct xfs_trans	*tp,
	struct xfs_btree_cur	*rcur,
	int			error)
{
	struct xfs_buf		*agbp;

	if (rcur == NULL)
		return;
	agbp = rcur->bc_private.a.agbp;
	xfs_btree_del_cursor(rcur, error ? XFS_BTREE_ERROR : XFS_BTREE_NOERROR);
	if (error)
		xfs_trans_brelse(tp, agbp);
}

/*
 * Process one of the deferred refcount operations.  We pass back the
 * btree cursor to maintain our lock on the btree between calls.
 * This saves time and eliminates a buffer deadlock between the
 * superblock and the AGF because we'll always grab them in the same
 * order.
 */
int
xfs_refcount_finish_one(
	struct xfs_trans		*tp,
	struct xfs_defer_ops		*dfops,
	enum xfs_refcount_intent_type	type,
	xfs_fsblock_t			startblock,
	xfs_extlen_t			blockcount,
	xfs_extlen_t			*adjusted,
	struct xfs_btree_cur		**pcur)
{
	struct xfs_mount		*mp = tp->t_mountp;
	struct xfs_btree_cur		*rcur;
	struct xfs_buf			*agbp = NULL;
	int				error = 0;
	xfs_agnumber_t			agno;
	xfs_agblock_t			bno;
	unsigned long			nr_ops = 0;
	int				shape_changes = 0;

	agno = XFS_FSB_TO_AGNO(mp, startblock);
	ASSERT(agno != NULLAGNUMBER);
	bno = XFS_FSB_TO_AGBNO(mp, startblock);

	trace_xfs_refcount_deferred(mp, XFS_FSB_TO_AGNO(mp, startblock),
			type, XFS_FSB_TO_AGBNO(mp, startblock),
			blockcount);

	if (XFS_TEST_ERROR(false, mp,
			XFS_ERRTAG_REFCOUNT_FINISH_ONE,
			XFS_RANDOM_REFCOUNT_FINISH_ONE))
		return -EIO;

	/*
	 * If we haven't gotten a cursor or the cursor AG doesn't match
	 * the startblock, get one now.
	 */
	rcur = *pcur;
	if (rcur != NULL && rcur->bc_private.a.agno != agno) {
		nr_ops = rcur->bc_private.a.priv.refc.nr_ops;
		shape_changes = rcur->bc_private.a.priv.refc.shape_changes;
		xfs_refcount_finish_one_cleanup(tp, rcur, 0);
		rcur = NULL;
		*pcur = NULL;
	}
	if (rcur == NULL) {
		error = xfs_alloc_read_agf(tp->t_mountp, tp, agno,
				XFS_ALLOC_FLAG_FREEING, &agbp);
		if (error)
			return error;
		if (!agbp)
			return -EFSCORRUPTED;

		rcur = xfs_refcountbt_init_cursor(mp, tp, agbp, agno, dfops);
		if (!rcur) {
			error = -ENOMEM;
			goto out_cur;
		}
		rcur->bc_private.a.priv.refc.nr_ops = nr_ops;
		rcur->bc_private.a.priv.refc.shape_changes = shape_changes;
	}
	*pcur = rcur;

	switch (type) {
	case XFS_REFCOUNT_INCREASE:
		error = xfs_refcount_adjust(rcur, bno, blockcount, adjusted,
			XFS_REFCOUNT_ADJUST_INCREASE, dfops, NULL);
		break;
	case XFS_REFCOUNT_DECREASE:
		error = xfs_refcount_adjust(rcur, bno, blockcount, adjusted,
			XFS_REFCOUNT_ADJUST_DECREASE, dfops, NULL);
		break;
	case XFS_REFCOUNT_ALLOC_COW:
		*adjusted = 0;
		error = __xfs_refcount_cow_alloc(rcur, bno, blockcount, dfops);
		if (!error)
			*adjusted = blockcount;
		break;
	case XFS_REFCOUNT_FREE_COW:
		*adjusted = 0;
		error = __xfs_refcount_cow_free(rcur, bno, blockcount, dfops);
		if (!error)
			*adjusted = blockcount;
		break;
	default:
		ASSERT(0);
		error = -EFSCORRUPTED;
	}
	if (!error && *adjusted != blockcount)
		trace_xfs_refcount_finish_one_leftover(mp, agno, type,
				bno, blockcount, *adjusted);
	return error;

out_cur:
	xfs_trans_brelse(tp, agbp);

	return error;
}

/*
 * Record a refcount intent for later processing.
 */
static int
__xfs_refcount_add(
	struct xfs_mount		*mp,
	struct xfs_defer_ops		*dfops,
	enum xfs_refcount_intent_type	type,
	xfs_fsblock_t			startblock,
	xfs_extlen_t			blockcount)
{
	struct xfs_refcount_intent	*ri;

	trace_xfs_refcount_defer(mp, XFS_FSB_TO_AGNO(mp, startblock),
			type, XFS_FSB_TO_AGBNO(mp, startblock),
			blockcount);

	ri = kmem_alloc(sizeof(struct xfs_refcount_intent),
			KM_SLEEP | KM_NOFS);
	INIT_LIST_HEAD(&ri->ri_list);
	ri->ri_type = type;
	ri->ri_startblock = startblock;
	ri->ri_blockcount = blockcount;

	xfs_defer_add(dfops, XFS_DEFER_OPS_TYPE_REFCOUNT, &ri->ri_list);
	return 0;
}

/*
 * Increase the reference count of the blocks backing a file's extent.
 */
int
xfs_refcount_increase_extent(
	struct xfs_mount		*mp,
	struct xfs_defer_ops		*dfops,
	struct xfs_bmbt_irec		*PREV)
{
	if (!xfs_sb_version_hasreflink(&mp->m_sb))
		return 0;

	return __xfs_refcount_add(mp, dfops, XFS_REFCOUNT_INCREASE,
			PREV->br_startblock, PREV->br_blockcount);
}

/*
 * Decrease the reference count of the blocks backing a file's extent.
 */
int
xfs_refcount_decrease_extent(
	struct xfs_mount		*mp,
	struct xfs_defer_ops		*dfops,
	struct xfs_bmbt_irec		*PREV)
{
	if (!xfs_sb_version_hasreflink(&mp->m_sb))
		return 0;

	return __xfs_refcount_add(mp, dfops, XFS_REFCOUNT_DECREASE,
			PREV->br_startblock, PREV->br_blockcount);
}

/*
 * Given an AG extent, find the lowest-numbered run of shared blocks within
 * that range and return the range in fbno/flen.  If find_maximal is set,
 * return the longest extent of shared blocks; if not, just return the first
 * extent we find.  If no shared blocks are found, flen will be set to zero.
 */
int
__xfs_refcount_find_shared(
	struct xfs_mount	*mp,
	struct xfs_buf		*agbp,
	xfs_agnumber_t		agno,
	xfs_agblock_t		agbno,
	xfs_extlen_t		aglen,
	xfs_agblock_t		*fbno,
	xfs_extlen_t		*flen,
	bool			find_maximal)
{
	struct xfs_btree_cur	*cur;
	struct xfs_refcount_irec	tmp;
	int			i, have;
	int			bt_error = XFS_BTREE_ERROR;
	int			error;

	trace_xfs_refcount_find_shared(mp, agno, agbno, aglen);

	cur = xfs_refcountbt_init_cursor(mp, NULL, agbp, agno, NULL);

	/* By default, skip the whole range */
	*fbno = agbno + aglen;
	*flen = 0;

	/* Try to find a refcount extent that crosses the start */
	error = xfs_refcount_lookup_le(cur, agbno, &have);
	if (error)
		goto out_error;
	if (!have) {
		/* No left extent, look at the next one */
		error = xfs_btree_increment(cur, 0, &have);
		if (error)
			goto out_error;
		if (!have)
			goto done;
	}
	error = xfs_refcount_get_rec(cur, &tmp, &i);
	if (error)
		goto out_error;
	XFS_WANT_CORRUPTED_GOTO(mp, i == 1, out_error);

	/* If the extent ends before the start, look at the next one */
	if (tmp.rc_startblock + tmp.rc_blockcount <= agbno) {
		error = xfs_btree_increment(cur, 0, &have);
		if (error)
			goto out_error;
		if (!have)
			goto done;
		error = xfs_refcount_get_rec(cur, &tmp, &i);
		if (error)
			goto out_error;
		XFS_WANT_CORRUPTED_GOTO(mp, i == 1, out_error);
	}

	/* If the extent ends after the range we want, bail out */
	if (tmp.rc_startblock >= agbno + aglen)
		goto done;

	/* We found the start of a shared extent! */
	if (tmp.rc_startblock < agbno) {
		tmp.rc_blockcount -= (agbno - tmp.rc_startblock);
		tmp.rc_startblock = agbno;
	}

	*fbno = tmp.rc_startblock;
	*flen = min(tmp.rc_blockcount, agbno + aglen - *fbno);
	if (!find_maximal)
		goto done;

	/* Otherwise, find the end of this shared extent */
	while (*fbno + *flen < agbno + aglen) {
		error = xfs_btree_increment(cur, 0, &have);
		if (error)
			goto out_error;
		if (!have)
			break;
		error = xfs_refcount_get_rec(cur, &tmp, &i);
		if (error)
			goto out_error;
		XFS_WANT_CORRUPTED_GOTO(mp, i == 1, out_error);
		if (tmp.rc_startblock >= agbno + aglen ||
		    tmp.rc_startblock != *fbno + *flen)
			break;
		*flen = min(*flen + tmp.rc_blockcount, agbno + aglen - *fbno);
	}

done:
	bt_error = XFS_BTREE_NOERROR;
	trace_xfs_refcount_find_shared_result(mp, agno, *fbno, *flen);

out_error:
	xfs_btree_del_cursor(cur, bt_error);
	if (error)
		trace_xfs_refcount_find_shared_error(mp, agno, error, _RET_IP_);
	return error;
}

/*
 * Given an AG extent, find the lowest-numbered run of shared blocks within
 * that range and return the range in fbno/flen.
 */
int
xfs_refcount_find_shared(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	xfs_agblock_t		agbno,
	xfs_extlen_t		aglen,
	xfs_agblock_t		*fbno,
	xfs_extlen_t		*flen,
	bool			find_maximal)
{
	struct xfs_buf		*agbp;
	int			error;

	if (xfs_always_cow) {
		*fbno = agbno;
		*flen = aglen;
		return 0;
	}

	error = xfs_alloc_read_agf(mp, NULL, agno, 0, &agbp);
	if (error)
		return error;

	error = __xfs_refcount_find_shared(mp, agbp, agno, agbno, aglen,
			fbno, flen, find_maximal);

	xfs_buf_relse(agbp);
	return error;
}

/*
 * Recovering CoW Blocks After a Crash
 *
 * Due to the way that the copy on write mechanism works, there's a window of
 * opportunity in which we can lose track of allocated blocks during a crash.
 * Because CoW uses delayed allocation in the in-core CoW fork, writeback
 * causes blocks to be allocated and stored in the CoW fork.  The blocks are
 * no longer in the free space btree but are not otherwise recorded anywhere
 * until the write completes and the blocks are mapped into the file.  A crash
 * in between allocation and remapping results in the replacement blocks being
 * lost.  This situation is exacerbated by the CoW extent size hint because
 * allocations can hang around for long time.
 *
 * However, there is a place where we can record these allocations before they
 * become mappings -- the reference count btree.  The btree does not record
 * extents with refcount == 1, so we can record allocations with a refcount of
 * 1.  Blocks being used for CoW writeout cannot be shared, so there should be
 * no conflict with shared block records.  These mappings should be created
 * when we allocate blocks to the CoW fork and deleted when they're removed
 * from the CoW fork.
 *
 * Minor nit: records for in-progress CoW allocations and records for shared
 * extents must never be merged, to preserve the property that (except for CoW
 * allocations) there are no refcount btree entries with refcount == 1.  The
 * only time this could potentially happen is when unsharing a block that's
 * adjacent to CoW allocations, so we must be careful to avoid this.
 *
 * At mount time we recover lost CoW allocations by searching the refcount
 * btree for these refcount == 1 mappings.  These represent CoW allocations
 * that were in progress at the time the filesystem went down, so we can free
 * them to get the space back.
 *
 * This mechanism is superior to creating EFIs for unmapped CoW extents for
 * several reasons -- first, EFIs pin the tail of the log and would have to be
 * periodically relogged to avoid filling up the log.  Second, CoW completions
 * will have to file an EFD and create new EFIs for whatever remains in the
 * CoW fork; this partially takes care of (1) but extent-size reservations
 * will have to periodically relog even if there's no writeout in progress.
 * This can happen if the CoW extent size hint is set, which you really want.
 * Third, EFIs cannot currently be automatically relogged into newer
 * transactions to advance the log tail.  Fourth, stuffing the log full of
 * EFIs places an upper bound on the number of CoW allocations that can be
 * held filesystem-wide at any given time.  Recording them in the refcount
 * btree doesn't require us to maintain any state in memory and doesn't pin
 * the log.
 */
/*
 * Adjust the refcounts of CoW allocations.  These allocations are "magic"
 * in that they're not referenced anywhere else in the filesystem, so we
 * stash them in the refcount btree with a refcount of 1 until either file
 * remapping (or CoW cancellation) happens.
 */
STATIC int
xfs_refcount_adjust_cow_extents(
	struct xfs_btree_cur	*cur,
	xfs_agblock_t		agbno,
	xfs_extlen_t		aglen,
	enum xfs_refc_adjust_op	adj,
	struct xfs_defer_ops	*dfops,
	struct xfs_owner_info	*oinfo)
{
	struct xfs_refcount_irec	ext, tmp;
	int				error;
	int				found_rec, found_tmp;

	if (aglen == 0)
		return 0;

	/* Find any overlapping refcount records */
	error = xfs_refcount_lookup_ge(cur, agbno, &found_rec);
	if (error)
		goto out_error;
	error = xfs_refcount_get_rec(cur, &ext, &found_rec);
	if (error)
		goto out_error;
	if (!found_rec) {
		ext.rc_startblock = cur->bc_mp->m_sb.sb_agblocks;
		ext.rc_blockcount = 0;
		ext.rc_refcount = 0;
	}

	switch (adj) {
	case XFS_REFCOUNT_ADJUST_COW_ALLOC:
		/* Adding a CoW reservation, there should be nothing here. */
		XFS_WANT_CORRUPTED_GOTO(cur->bc_mp,
				ext.rc_startblock >= agbno + aglen, out_error);

		tmp.rc_startblock = agbno;
		tmp.rc_blockcount = aglen;
		tmp.rc_refcount = 1;
		trace_xfs_refcount_modify_extent(cur->bc_mp,
				cur->bc_private.a.agno, &tmp);

		error = xfs_refcount_insert(cur, &tmp,
				&found_tmp);
		if (error)
			goto out_error;
		XFS_WANT_CORRUPTED_GOTO(cur->bc_mp,
				found_tmp == 1, out_error);
		break;
	case XFS_REFCOUNT_ADJUST_COW_FREE:
		/* Removing a CoW reservation, there should be one extent. */
		XFS_WANT_CORRUPTED_GOTO(cur->bc_mp,
			ext.rc_startblock == agbno, out_error);
		XFS_WANT_CORRUPTED_GOTO(cur->bc_mp,
			ext.rc_blockcount == aglen, out_error);
		XFS_WANT_CORRUPTED_GOTO(cur->bc_mp,
			ext.rc_refcount == 1, out_error);

		ext.rc_refcount = 0;
		trace_xfs_refcount_modify_extent(cur->bc_mp,
				cur->bc_private.a.agno, &ext);
		error = xfs_refcount_delete(cur, &found_rec);
		if (error)
			goto out_error;
		XFS_WANT_CORRUPTED_GOTO(cur->bc_mp,
				found_rec == 1, out_error);
		break;
	default:
		ASSERT(0);
	}

	return error;
out_error:
	trace_xfs_refcount_modify_extent_error(cur->bc_mp,
			cur->bc_private.a.agno, error, _RET_IP_);
	return error;
}

/*
 * Add or remove refcount btree entries for CoW reservations.
 */
STATIC int
xfs_refcount_adjust_cow(
	struct xfs_btree_cur	*cur,
	xfs_agblock_t		agbno,
	xfs_extlen_t		aglen,
	enum xfs_refc_adjust_op	adj,
	struct xfs_defer_ops	*dfops)
{
	bool			shape_changed;
	int			error;

	/*
	 * Ensure that no rcextents cross the boundary of the adjustment range.
	 */
	error = xfs_refcount_split_extent(cur, agbno, &shape_changed);
	if (error)
		goto out_error;

	error = xfs_refcount_split_extent(cur, agbno + aglen, &shape_changed);
	if (error)
		goto out_error;

	/*
	 * Try to merge with the left or right extents of the range.
	 */
	error = xfs_refcount_merge_extents(cur, &agbno, &aglen, adj,
			XFS_FIND_RCEXT_COW, &shape_changed);
	if (error)
		goto out_error;

	/* Now that we've taken care of the ends, adjust the middle extents */
	error = xfs_refcount_adjust_cow_extents(cur, agbno, aglen, adj,
			dfops, NULL);
	if (error)
		goto out_error;

	return 0;

out_error:
	trace_xfs_refcount_adjust_cow_error(cur->bc_mp, cur->bc_private.a.agno,
			error, _RET_IP_);
	return error;
}

/*
 * Record a CoW allocation in the refcount btree.
 */
STATIC int
__xfs_refcount_cow_alloc(
	struct xfs_btree_cur	*rcur,
	xfs_agblock_t		agbno,
	xfs_extlen_t		aglen,
	struct xfs_defer_ops	*dfops)
{
	int			error;

	trace_xfs_refcount_cow_increase(rcur->bc_mp, rcur->bc_private.a.agno,
			agbno, aglen);

	/* Add refcount btree reservation */
	error = xfs_refcount_adjust_cow(rcur, agbno, aglen,
			XFS_REFCOUNT_ADJUST_COW_ALLOC, dfops);
	if (error)
		return error;

	/* Add rmap entry */
	if (xfs_sb_version_hasrmapbt(&rcur->bc_mp->m_sb)) {
		error = xfs_rmap_alloc_extent(rcur->bc_mp, dfops,
				rcur->bc_private.a.agno,
				agbno, aglen, XFS_RMAP_OWN_COW);
		if (error)
			return error;
	}

	return error;
}

/*
 * Remove a CoW allocation from the refcount btree.
 */
STATIC int
__xfs_refcount_cow_free(
	struct xfs_btree_cur	*rcur,
	xfs_agblock_t		agbno,
	xfs_extlen_t		aglen,
	struct xfs_defer_ops	*dfops)
{
	int			error;

	trace_xfs_refcount_cow_decrease(rcur->bc_mp, rcur->bc_private.a.agno,
			agbno, aglen);

	/* Remove refcount btree reservation */
	error = xfs_refcount_adjust_cow(rcur, agbno, aglen,
			XFS_REFCOUNT_ADJUST_COW_FREE, dfops);
	if (error)
		return error;

	/* Remove rmap entry */
	if (xfs_sb_version_hasrmapbt(&rcur->bc_mp->m_sb)) {
		error = xfs_rmap_free_extent(rcur->bc_mp, dfops,
				rcur->bc_private.a.agno,
				agbno, aglen, XFS_RMAP_OWN_COW);
		if (error)
			return error;
	}

	return error;
}

/* Record a CoW staging extent in the refcount btree. */
int
xfs_refcount_alloc_cow_extent(
	struct xfs_mount		*mp,
	struct xfs_defer_ops		*dfops,
	xfs_fsblock_t			fsb,
	xfs_extlen_t			len)
{
	if (!xfs_sb_version_hasreflink(&mp->m_sb))
		return 0;

	return __xfs_refcount_add(mp, dfops, XFS_REFCOUNT_ALLOC_COW,
			fsb, len);
}

/* Forget a CoW staging event in the refcount btree. */
int
xfs_refcount_free_cow_extent(
	struct xfs_mount		*mp,
	struct xfs_defer_ops		*dfops,
	xfs_fsblock_t			fsb,
	xfs_extlen_t			len)
{
	if (!xfs_sb_version_hasreflink(&mp->m_sb))
		return 0;

	return __xfs_refcount_add(mp, dfops, XFS_REFCOUNT_FREE_COW,
			fsb, len);
}

struct xfs_refcountbt_scrub_fragment {
	struct xfs_rmap_irec		rm;
	struct list_head		list;
};

struct xfs_refcountbt_scrub_rmap_check_info {
	xfs_nlink_t			nr;
	struct xfs_refcount_irec	rc;
	struct list_head		fragments;
};

static int
xfs_refcountbt_scrub_rmap_check(
	struct xfs_btree_cur		*cur,
	struct xfs_rmap_irec		*rec,
	void				*priv)
{
	struct xfs_refcountbt_scrub_rmap_check_info	*rsrci = priv;
	struct xfs_refcountbt_scrub_fragment		*frag;
	xfs_agblock_t			rm_last;
	xfs_agblock_t			rc_last;

	rm_last = rec->rm_startblock + rec->rm_blockcount;
	rc_last = rsrci->rc.rc_startblock + rsrci->rc.rc_blockcount;
	if (rec->rm_startblock <= rsrci->rc.rc_startblock && rm_last >= rc_last)
		rsrci->nr++;
	else {
		frag = kmem_zalloc(sizeof(struct xfs_refcountbt_scrub_fragment),
				KM_SLEEP);
		frag->rm = *rec;
		list_add_tail(&frag->list, &rsrci->fragments);
	}

	return 0;
}

STATIC void
xfs_refcountbt_process_rmap_fragments(
	struct xfs_mount				*mp,
	struct xfs_refcountbt_scrub_rmap_check_info	*rsrci)
{
	struct list_head				worklist;
	struct xfs_refcountbt_scrub_fragment		*cur;
	struct xfs_refcountbt_scrub_fragment		*n;
	xfs_agblock_t					bno;
	xfs_agblock_t					rbno;
	xfs_agblock_t					next_rbno;
	xfs_nlink_t					nr;
	xfs_nlink_t					target_nr;

	target_nr = rsrci->rc.rc_refcount - rsrci->nr;
	if (target_nr == 0)
		return;

	/*
	 * There are (rsrci->rc.rc_refcount - rsrci->nr refcount)
	 * references we haven't found yet.  Pull that many off the
	 * fragment list and figure out where the smallest rmap ends
	 * (and therefore the next rmap should start).  All the rmaps
	 * we pull off should start at or before the beginning of the
	 * refcount record's range.
	 */
	INIT_LIST_HEAD(&worklist);
	rbno = NULLAGBLOCK;
	nr = 1;
	list_for_each_entry_safe(cur, n, &rsrci->fragments, list) {
		if (cur->rm.rm_startblock > rsrci->rc.rc_startblock)
			goto fail;
		bno = cur->rm.rm_startblock + cur->rm.rm_blockcount;
		if (rbno > bno)
			rbno = bno;
		list_del(&cur->list);
		list_add_tail(&cur->list, &worklist);
		if (nr == target_nr)
			break;
		nr++;
	}

	if (nr != target_nr)
		goto fail;

	while (!list_empty(&rsrci->fragments)) {
		/* Discard any fragments ending at rbno. */
		nr = 0;
		next_rbno = NULLAGBLOCK;
		list_for_each_entry_safe(cur, n, &worklist, list) {
			bno = cur->rm.rm_startblock + cur->rm.rm_blockcount;
			if (bno != rbno) {
				if (next_rbno > bno)
					next_rbno = bno;
				continue;
			}
			list_del(&cur->list);
			kmem_free(cur);
			nr++;
		}

		/* Empty list?  We're done. */
		if (list_empty(&rsrci->fragments))
			break;

		/* Try to add nr rmaps starting at rbno to the worklist. */
		list_for_each_entry_safe(cur, n, &rsrci->fragments, list) {
			bno = cur->rm.rm_startblock + cur->rm.rm_blockcount;
			if (cur->rm.rm_startblock != rbno)
				goto fail;
			list_del(&cur->list);
			list_add_tail(&cur->list, &worklist);
			if (next_rbno > bno)
				next_rbno = bno;
			nr--;
			if (nr == 0)
				break;
		}

		rbno = next_rbno;
	}

	/*
	 * Make sure the last extent we processed ends at or beyond
	 * the end of the refcount extent.
	 */
	if (rbno < rsrci->rc.rc_startblock + rsrci->rc.rc_blockcount)
		goto fail;

	rsrci->nr = rsrci->rc.rc_refcount;
fail:
	/* Delete fragments and work list. */
	while (!list_empty(&worklist)) {
		cur = list_first_entry(&worklist,
				struct xfs_refcountbt_scrub_fragment, list);
		list_del(&cur->list);
		kmem_free(cur);
	}
	while (!list_empty(&rsrci->fragments)) {
		cur = list_first_entry(&rsrci->fragments,
				struct xfs_refcountbt_scrub_fragment, list);
		list_del(&cur->list);
		kmem_free(cur);
	}
}

STATIC int
xfs_refcountbt_scrub_helper(
	struct xfs_btree_scrub		*bs,
	union xfs_btree_rec		*rec)
{
	struct xfs_mount		*mp = bs->cur->bc_mp;
	struct xfs_rmap_irec		low;
	struct xfs_rmap_irec		high;
	struct xfs_refcount_irec	irec;
	struct xfs_refcountbt_scrub_rmap_check_info	rsrci;
	struct xfs_refcountbt_scrub_fragment		*cur;
	int				error;

	irec.rc_startblock = be32_to_cpu(rec->refc.rc_startblock);
	irec.rc_blockcount = be32_to_cpu(rec->refc.rc_blockcount);
	irec.rc_refcount = be32_to_cpu(rec->refc.rc_refcount);

	XFS_BTREC_SCRUB_CHECK(bs, irec.rc_startblock < mp->m_sb.sb_agblocks);
	XFS_BTREC_SCRUB_CHECK(bs, irec.rc_startblock < irec.rc_startblock +
			irec.rc_blockcount);
	XFS_BTREC_SCRUB_CHECK(bs, (unsigned long long)irec.rc_startblock +
			irec.rc_blockcount <= mp->m_sb.sb_agblocks);
	XFS_BTREC_SCRUB_CHECK(bs, irec.rc_refcount >= 1);

	/* confirm the refcount */
	if (!bs->rmap_cur)
		return 0;

	memset(&low, 0, sizeof(low));
	low.rm_startblock = irec.rc_startblock;
	memset(&high, 0xFF, sizeof(high));
	high.rm_startblock = irec.rc_startblock + irec.rc_blockcount - 1;

	rsrci.nr = 0;
	rsrci.rc = irec;
	INIT_LIST_HEAD(&rsrci.fragments);
	error = xfs_rmap_query_range(bs->rmap_cur, &low, &high,
			&xfs_refcountbt_scrub_rmap_check, &rsrci);
	if (error && error != XFS_BTREE_QUERY_RANGE_ABORT)
		goto err;
	error = 0;
	xfs_refcountbt_process_rmap_fragments(mp, &rsrci);
	XFS_BTREC_SCRUB_CHECK(bs, irec.rc_refcount == rsrci.nr);

err:
	while (!list_empty(&rsrci.fragments)) {
		cur = list_first_entry(&rsrci.fragments,
				struct xfs_refcountbt_scrub_fragment, list);
		list_del(&cur->list);
		kmem_free(cur);
	}
	return error;
}

/* Scrub the refcount btree for some AG. */
int
xfs_refcountbt_scrub(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno)
{
	struct xfs_btree_scrub	bs;
	int			error;

	error = xfs_alloc_read_agf(mp, NULL, agno, 0, &bs.agf_bp);
	if (error)
		return error;

	bs.cur = xfs_refcountbt_init_cursor(mp, NULL, bs.agf_bp, agno, NULL);
	bs.scrub_rec = xfs_refcountbt_scrub_helper;
	xfs_rmap_ag_owner(&bs.oinfo, XFS_RMAP_OWN_REFC);
	error = xfs_btree_scrub(&bs);
	xfs_btree_del_cursor(bs.cur,
			error ? XFS_BTREE_ERROR : XFS_BTREE_NOERROR);
	xfs_trans_brelse(NULL, bs.agf_bp);

	if (!error && bs.error)
		error = bs.error;

	return error;
}
