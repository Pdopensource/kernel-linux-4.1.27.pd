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
#ifndef __XFS_REFLINK_H
#define __XFS_REFLINK_H 1

extern int xfs_reflink_irec_is_shared(struct xfs_inode *ip,
		struct xfs_bmbt_irec *imap, bool *shared);
extern int xfs_reflink_reserve_cow_range(struct xfs_inode *ip,
		xfs_fileoff_t offset_fsb, xfs_fileoff_t end_fsb);
extern int xfs_reflink_allocate_cow_range(struct xfs_inode *ip, xfs_off_t pos,
		xfs_off_t len);
extern bool xfs_reflink_is_cow_pending(struct xfs_inode *ip, xfs_off_t offset);
extern int xfs_reflink_find_cow_mapping(struct xfs_inode *ip, xfs_off_t offset,
		struct xfs_bmbt_irec *imap, bool *need_alloc);
extern int xfs_reflink_trim_irec_to_next_cow(struct xfs_inode *ip,
		xfs_fileoff_t offset_fsb, struct xfs_bmbt_irec *imap);

extern int xfs_reflink_cancel_cow_blocks(struct xfs_inode *ip,
		struct xfs_trans **tpp, xfs_fileoff_t offset_fsb,
		xfs_fileoff_t end_fsb);
extern int xfs_reflink_cancel_cow_range(struct xfs_inode *ip, xfs_off_t offset,
		xfs_off_t count);
extern int xfs_reflink_end_cow(struct xfs_inode *ip, xfs_off_t offset,
		xfs_off_t count);
extern int xfs_reflink_recover_cow(struct xfs_mount *mp);

#define XFS_REFLINK_DEDUPE	1	/* only reflink if contents match */
#define XFS_REFLINK_ALL		(XFS_REFLINK_DEDUPE)

extern int xfs_reflink_remap_range(struct xfs_inode *src, xfs_off_t srcoff,
		struct xfs_inode *dest, xfs_off_t destoff, xfs_off_t len,
		unsigned int flags);
extern int xfs_reflink_unshare(struct xfs_inode *ip, xfs_off_t offset,
		xfs_off_t len);
extern int xfs_reflink_cow_eof_block(struct xfs_inode *ip, xfs_off_t newsize);
extern void xfs_reflink_get_lxflags(struct xfs_inode *ip, unsigned int *flags);
extern int xfs_reflink_check_flag_adjust(struct xfs_inode *ip,
		unsigned int *xflags);
extern bool xfs_reflink_has_real_cow_blocks(struct xfs_inode *ip);

#endif /* __XFS_REFLINK_H */
