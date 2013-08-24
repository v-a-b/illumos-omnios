/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 *
 * Portions Copyright 2010 Robert Milkowski
 *
 * Copyright 2011 Nexenta Systems, Inc.  All rights reserved.
 * Copyright (c) 2013 by Delphix. All rights reserved.
 * Copyright (c) 2013, Joyent, Inc. All rights reserved.
 */

/*
 * ZFS volume emulation driver.
 *
 * Makes a DMU object look like a volume of arbitrary size, up to 2^64 bytes.
 * Volumes are accessed through the symbolic links named:
 *
 * /dev/zvol/dsk/<pool_name>/<dataset_name>
 * /dev/zvol/rdsk/<pool_name>/<dataset_name>
 *
 * These links are created by the /dev filesystem (sdev_zvolops.c).
 * Volumes are persistent through reboot.  No user command needs to be
 * run before opening and using a device.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/uio.h>
#include <sys/buf.h>
#include <sys/modctl.h>
#include <sys/open.h>
#include <sys/kmem.h>
#include <sys/conf.h>
#include <sys/cmn_err.h>
#include <sys/stat.h>
#include <sys/zap.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/zio.h>
#include <sys/dmu_traverse.h>
#include <sys/dnode.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_prop.h>
#include <sys/dkio.h>
#include <sys/efi_partition.h>
#include <sys/byteorder.h>
#include <sys/pathname.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/crc32.h>
#include <sys/dirent.h>
#include <sys/policy.h>
#include <sys/fs/zfs.h>
#include <sys/zfs_ioctl.h>
#include <sys/mkdev.h>
#include <sys/zil.h>
#include <sys/refcount.h>
#include <sys/zfs_znode.h>
#include <sys/zfs_rlock.h>
#include <sys/vdev_disk.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_raidz.h>
#include <sys/zvol.h>
#include <sys/dumphdr.h>
#include <sys/zil_impl.h>
#include <sys/sdt.h>
#include <sys/dbuf.h>
#include <sys/dmu_tx.h>
#include <sys/zfeature.h>
#include <sys/zio_checksum.h>

#include "zfs_namecheck.h"

void *zfsdev_state;
static char *zvol_tag = "zvol_tag";

#define	ZVOL_DUMPSIZE		"dumpsize"

/*
 * This lock protects the zfsdev_state structure from being modified
 * while it's being used, e.g. an open that comes in before a create
 * finishes.  It also protects temporary opens of the dataset so that,
 * e.g., an open doesn't get a spurious EBUSY.
 */
kmutex_t zfsdev_state_lock;
static uint32_t zvol_minors;

typedef struct zvol_extent {
	list_node_t	ze_node;
	dva_t		ze_dva;		/* dva associated with this extent */
	uint64_t	ze_nblks;	/* number of blocks in extent */
} zvol_extent_t;

/*
 * The in-core state of each volume.
 */
typedef struct zvol_state {
	char		zv_name[MAXPATHLEN]; /* pool/dd name */
	uint64_t	zv_volsize;	/* amount of space we advertise */
	uint64_t	zv_volblocksize; /* volume block size */
	minor_t		zv_minor;	/* minor number */
	uint8_t		zv_min_bs;	/* minimum addressable block shift */
	uint8_t		zv_flags;	/* readonly, dumpified, etc. */
	objset_t	*zv_objset;	/* objset handle */
	uint32_t	zv_open_count[OTYPCNT];	/* open counts */
	uint32_t	zv_total_opens;	/* total open count */
	zilog_t		*zv_zilog;	/* ZIL handle */
	list_t		zv_extents;	/* List of extents for dump */
	znode_t		zv_znode;	/* for range locking */
	dmu_buf_t	*zv_dbuf;	/* bonus handle */
} zvol_state_t;

/*
 * zvol specific flags
 */
#define	ZVOL_RDONLY	0x1
#define	ZVOL_DUMPIFIED	0x2
#define	ZVOL_EXCL	0x4
#define	ZVOL_WCE	0x8

/*
 * zvol maximum transfer in one DMU tx.
 */
int zvol_maxphys = DMU_MAX_ACCESS/2;

extern int zfs_set_prop_nvlist(const char *, zprop_source_t,
    nvlist_t *, nvlist_t *);
static int zvol_remove_zv(zvol_state_t *);
static int zvol_get_data(void *arg, lr_write_t *lr, char *buf, zio_t *zio);
static int zvol_dumpify(zvol_state_t *zv);
static int zvol_dump_fini(zvol_state_t *zv);
static int zvol_dump_init(zvol_state_t *zv, boolean_t resize);

static void
zvol_size_changed(zvol_state_t *zv, uint64_t volsize)
{
	dev_t dev = makedevice(ddi_driver_major(zfs_dip), zv->zv_minor);

	zv->zv_volsize = volsize;
	VERIFY(ddi_prop_update_int64(dev, zfs_dip,
	    "Size", volsize) == DDI_SUCCESS);
	VERIFY(ddi_prop_update_int64(dev, zfs_dip,
	    "Nblocks", lbtodb(volsize)) == DDI_SUCCESS);

	/* Notify specfs to invalidate the cached size */
	spec_size_invalidate(dev, VBLK);
	spec_size_invalidate(dev, VCHR);
}

int
zvol_check_volsize(uint64_t volsize, uint64_t blocksize)
{
	if (volsize == 0)
		return (SET_ERROR(EINVAL));

	if (volsize % blocksize != 0)
		return (SET_ERROR(EINVAL));

#ifdef _ILP32
	if (volsize - 1 > SPEC_MAXOFFSET_T)
		return (SET_ERROR(EOVERFLOW));
#endif
	return (0);
}

int
zvol_check_volblocksize(uint64_t volblocksize)
{
	if (volblocksize < SPA_MINBLOCKSIZE ||
	    volblocksize > SPA_MAXBLOCKSIZE ||
	    !ISP2(volblocksize))
		return (SET_ERROR(EDOM));

	return (0);
}

int
zvol_get_stats(objset_t *os, nvlist_t *nv)
{
	int error;
	dmu_object_info_t doi;
	uint64_t val;

	error = zap_lookup(os, ZVOL_ZAP_OBJ, "size", 8, 1, &val);
	if (error)
		return (error);

	dsl_prop_nvlist_add_uint64(nv, ZFS_PROP_VOLSIZE, val);

	error = dmu_object_info(os, ZVOL_OBJ, &doi);

	if (error == 0) {
		dsl_prop_nvlist_add_uint64(nv, ZFS_PROP_VOLBLOCKSIZE,
		    doi.doi_data_block_size);
	}

	return (error);
}

static zvol_state_t *
zvol_minor_lookup(const char *name)
{
	minor_t minor;
	zvol_state_t *zv;

	ASSERT(MUTEX_HELD(&zfsdev_state_lock));

	for (minor = 1; minor <= ZFSDEV_MAX_MINOR; minor++) {
		zv = zfsdev_get_soft_state(minor, ZSST_ZVOL);
		if (zv == NULL)
			continue;
		if (strcmp(zv->zv_name, name) == 0)
			return (zv);
	}

	return (NULL);
}

/* extent mapping arg */
struct maparg {
	zvol_state_t	*ma_zv;
	uint64_t	ma_blks;
};

/*ARGSUSED*/
static int
zvol_map_block(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_t *zb, const dnode_phys_t *dnp, void *arg)
{
	struct maparg *ma = arg;
	zvol_extent_t *ze;
	int bs = ma->ma_zv->zv_volblocksize;

	if (bp == NULL || zb->zb_object != ZVOL_OBJ || zb->zb_level != 0)
		return (0);

	VERIFY3U(ma->ma_blks, ==, zb->zb_blkid);
	ma->ma_blks++;

	/* Abort immediately if we have encountered gang blocks */
	if (BP_IS_GANG(bp))
		return (SET_ERROR(EFRAGS));

	/*
	 * See if the block is at the end of the previous extent.
	 */
	ze = list_tail(&ma->ma_zv->zv_extents);
	if (ze &&
	    DVA_GET_VDEV(BP_IDENTITY(bp)) == DVA_GET_VDEV(&ze->ze_dva) &&
	    DVA_GET_OFFSET(BP_IDENTITY(bp)) ==
	    DVA_GET_OFFSET(&ze->ze_dva) + ze->ze_nblks * bs) {
		ze->ze_nblks++;
		return (0);
	}

	dprintf_bp(bp, "%s", "next blkptr:");

	/* start a new extent */
	ze = kmem_zalloc(sizeof (zvol_extent_t), KM_SLEEP);
	ze->ze_dva = bp->blk_dva[0];	/* structure assignment */
	ze->ze_nblks = 1;
	list_insert_tail(&ma->ma_zv->zv_extents, ze);
	return (0);
}

static void
zvol_free_extents(zvol_state_t *zv)
{
	zvol_extent_t *ze;

	while (ze = list_head(&zv->zv_extents)) {
		list_remove(&zv->zv_extents, ze);
		kmem_free(ze, sizeof (zvol_extent_t));
	}
}

static int
zvol_get_lbas(zvol_state_t *zv)
{
	objset_t *os = zv->zv_objset;
	struct maparg	ma;
	int		err;

	ma.ma_zv = zv;
	ma.ma_blks = 0;
	zvol_free_extents(zv);

	/* commit any in-flight changes before traversing the dataset */
	txg_wait_synced(dmu_objset_pool(os), 0);
	err = traverse_dataset(dmu_objset_ds(os), 0,
	    TRAVERSE_PRE | TRAVERSE_PREFETCH_METADATA, zvol_map_block, &ma);
	if (err || ma.ma_blks != (zv->zv_volsize / zv->zv_volblocksize)) {
		zvol_free_extents(zv);
		return (err ? err : EIO);
	}

	return (0);
}

/* ARGSUSED */
void
zvol_create_cb(objset_t *os, void *arg, cred_t *cr, dmu_tx_t *tx)
{
	zfs_creat_t *zct = arg;
	nvlist_t *nvprops = zct->zct_props;
	int error;
	uint64_t volblocksize, volsize;

	VERIFY(nvlist_lookup_uint64(nvprops,
	    zfs_prop_to_name(ZFS_PROP_VOLSIZE), &volsize) == 0);
	if (nvlist_lookup_uint64(nvprops,
	    zfs_prop_to_name(ZFS_PROP_VOLBLOCKSIZE), &volblocksize) != 0)
		volblocksize = zfs_prop_default_numeric(ZFS_PROP_VOLBLOCKSIZE);

	/*
	 * These properties must be removed from the list so the generic
	 * property setting step won't apply to them.
	 */
	VERIFY(nvlist_remove_all(nvprops,
	    zfs_prop_to_name(ZFS_PROP_VOLSIZE)) == 0);
	(void) nvlist_remove_all(nvprops,
	    zfs_prop_to_name(ZFS_PROP_VOLBLOCKSIZE));

	error = dmu_object_claim(os, ZVOL_OBJ, DMU_OT_ZVOL, volblocksize,
	    DMU_OT_NONE, 0, tx);
	ASSERT(error == 0);

	error = zap_create_claim(os, ZVOL_ZAP_OBJ, DMU_OT_ZVOL_PROP,
	    DMU_OT_NONE, 0, tx);
	ASSERT(error == 0);

	error = zap_update(os, ZVOL_ZAP_OBJ, "size", 8, 1, &volsize, tx);
	ASSERT(error == 0);
}

/*
 * Replay a TX_TRUNCATE ZIL transaction if asked.  TX_TRUNCATE is how we
 * implement DKIOCFREE/free-long-range.
 */
static int
zvol_replay_truncate(zvol_state_t *zv, lr_truncate_t *lr, boolean_t byteswap)
{
	uint64_t offset, length;

	if (byteswap)
		byteswap_uint64_array(lr, sizeof (*lr));

	offset = lr->lr_offset;
	length = lr->lr_length;

	return (dmu_free_long_range(zv->zv_objset, ZVOL_OBJ, offset, length));
}

/*
 * Replay a TX_WRITE ZIL transaction that didn't get committed
 * after a system failure
 */
static int
zvol_replay_write(zvol_state_t *zv, lr_write_t *lr, boolean_t byteswap)
{
	objset_t *os = zv->zv_objset;
	char *data = (char *)(lr + 1);	/* data follows lr_write_t */
	uint64_t offset, length;
	dmu_tx_t *tx;
	int error;

	if (byteswap)
		byteswap_uint64_array(lr, sizeof (*lr));

	offset = lr->lr_offset;
	length = lr->lr_length;

	/* If it's a dmu_sync() block, write the whole block */
	if (lr->lr_common.lrc_reclen == sizeof (lr_write_t)) {
		uint64_t blocksize = BP_GET_LSIZE(&lr->lr_blkptr);
		if (length < blocksize) {
			offset -= offset % blocksize;
			length = blocksize;
		}
	}

	tx = dmu_tx_create(os);
	dmu_tx_hold_write(tx, ZVOL_OBJ, offset, length);
	error = dmu_tx_assign(tx, TXG_WAIT);
	if (error) {
		dmu_tx_abort(tx);
	} else {
		dmu_write(os, ZVOL_OBJ, offset, length, data, tx);
		dmu_tx_commit(tx);
	}

	return (error);
}

/* ARGSUSED */
static int
zvol_replay_err(zvol_state_t *zv, lr_t *lr, boolean_t byteswap)
{
	return (SET_ERROR(ENOTSUP));
}

/*
 * Callback vectors for replaying records.
 * Only TX_WRITE and TX_TRUNCATE are needed for zvol.
 */
zil_replay_func_t *zvol_replay_vector[TX_MAX_TYPE] = {
	zvol_replay_err,	/* 0 no such transaction type */
	zvol_replay_err,	/* TX_CREATE */
	zvol_replay_err,	/* TX_MKDIR */
	zvol_replay_err,	/* TX_MKXATTR */
	zvol_replay_err,	/* TX_SYMLINK */
	zvol_replay_err,	/* TX_REMOVE */
	zvol_replay_err,	/* TX_RMDIR */
	zvol_replay_err,	/* TX_LINK */
	zvol_replay_err,	/* TX_RENAME */
	zvol_replay_write,	/* TX_WRITE */
	zvol_replay_truncate,	/* TX_TRUNCATE */
	zvol_replay_err,	/* TX_SETATTR */
	zvol_replay_err,	/* TX_ACL */
	zvol_replay_err,	/* TX_CREATE_ACL */
	zvol_replay_err,	/* TX_CREATE_ATTR */
	zvol_replay_err,	/* TX_CREATE_ACL_ATTR */
	zvol_replay_err,	/* TX_MKDIR_ACL */
	zvol_replay_err,	/* TX_MKDIR_ATTR */
	zvol_replay_err,	/* TX_MKDIR_ACL_ATTR */
	zvol_replay_err,	/* TX_WRITE2 */
};

int
zvol_name2minor(const char *name, minor_t *minor)
{
	zvol_state_t *zv;

	mutex_enter(&zfsdev_state_lock);
	zv = zvol_minor_lookup(name);
	if (minor && zv)
		*minor = zv->zv_minor;
	mutex_exit(&zfsdev_state_lock);
	return (zv ? 0 : -1);
}

/*
 * Create a minor node (plus a whole lot more) for the specified volume.
 */
int
zvol_create_minor(const char *name)
{
	zfs_soft_state_t *zs;
	zvol_state_t *zv;
	objset_t *os;
	dmu_object_info_t doi;
	minor_t minor = 0;
	char chrbuf[30], blkbuf[30];
	int error;

	mutex_enter(&zfsdev_state_lock);

	if (zvol_minor_lookup(name) != NULL) {
		mutex_exit(&zfsdev_state_lock);
		return (SET_ERROR(EEXIST));
	}

	/* lie and say we're read-only */
	error = dmu_objset_own(name, DMU_OST_ZVOL, B_TRUE, FTAG, &os);

	if (error) {
		mutex_exit(&zfsdev_state_lock);
		return (error);
	}

	if ((minor = zfsdev_minor_alloc()) == 0) {
		dmu_objset_disown(os, FTAG);
		mutex_exit(&zfsdev_state_lock);
		return (SET_ERROR(ENXIO));
	}

	if (ddi_soft_state_zalloc(zfsdev_state, minor) != DDI_SUCCESS) {
		dmu_objset_disown(os, FTAG);
		mutex_exit(&zfsdev_state_lock);
		return (SET_ERROR(EAGAIN));
	}
	(void) ddi_prop_update_string(minor, zfs_dip, ZVOL_PROP_NAME,
	    (char *)name);

	(void) snprintf(chrbuf, sizeof (chrbuf), "%u,raw", minor);

	if (ddi_create_minor_node(zfs_dip, chrbuf, S_IFCHR,
	    minor, DDI_PSEUDO, 0) == DDI_FAILURE) {
		ddi_soft_state_free(zfsdev_state, minor);
		dmu_objset_disown(os, FTAG);
		mutex_exit(&zfsdev_state_lock);
		return (SET_ERROR(EAGAIN));
	}

	(void) snprintf(blkbuf, sizeof (blkbuf), "%u", minor);

	if (ddi_create_minor_node(zfs_dip, blkbuf, S_IFBLK,
	    minor, DDI_PSEUDO, 0) == DDI_FAILURE) {
		ddi_remove_minor_node(zfs_dip, chrbuf);
		ddi_soft_state_free(zfsdev_state, minor);
		dmu_objset_disown(os, FTAG);
		mutex_exit(&zfsdev_state_lock);
		return (SET_ERROR(EAGAIN));
	}

	zs = ddi_get_soft_state(zfsdev_state, minor);
	zs->zss_type = ZSST_ZVOL;
	zv = zs->zss_data = kmem_zalloc(sizeof (zvol_state_t), KM_SLEEP);
	(void) strlcpy(zv->zv_name, name, MAXPATHLEN);
	zv->zv_min_bs = DEV_BSHIFT;
	zv->zv_minor = minor;
	zv->zv_objset = os;
	if (dmu_objset_is_snapshot(os) || !spa_writeable(dmu_objset_spa(os)))
		zv->zv_flags |= ZVOL_RDONLY;
	mutex_init(&zv->zv_znode.z_range_lock, NULL, MUTEX_DEFAULT, NULL);
	avl_create(&zv->zv_znode.z_range_avl, zfs_range_compare,
	    sizeof (rl_t), offsetof(rl_t, r_node));
	list_create(&zv->zv_extents, sizeof (zvol_extent_t),
	    offsetof(zvol_extent_t, ze_node));
	/* get and cache the blocksize */
	error = dmu_object_info(os, ZVOL_OBJ, &doi);
	ASSERT(error == 0);
	zv->zv_volblocksize = doi.doi_data_block_size;

	if (spa_writeable(dmu_objset_spa(os))) {
		if (zil_replay_disable)
			zil_destroy(dmu_objset_zil(os), B_FALSE);
		else
			zil_replay(os, zv, zvol_replay_vector);
	}
	dmu_objset_disown(os, FTAG);
	zv->zv_objset = NULL;

	zvol_minors++;

	mutex_exit(&zfsdev_state_lock);

	return (0);
}

/*
 * Remove minor node for the specified volume.
 */
static int
zvol_remove_zv(zvol_state_t *zv)
{
	char nmbuf[20];
	minor_t minor = zv->zv_minor;

	ASSERT(MUTEX_HELD(&zfsdev_state_lock));
	if (zv->zv_total_opens != 0)
		return (SET_ERROR(EBUSY));

	(void) snprintf(nmbuf, sizeof (nmbuf), "%u,raw", minor);
	ddi_remove_minor_node(zfs_dip, nmbuf);

	(void) snprintf(nmbuf, sizeof (nmbuf), "%u", minor);
	ddi_remove_minor_node(zfs_dip, nmbuf);

	avl_destroy(&zv->zv_znode.z_range_avl);
	mutex_destroy(&zv->zv_znode.z_range_lock);

	kmem_free(zv, sizeof (zvol_state_t));

	ddi_soft_state_free(zfsdev_state, minor);

	zvol_minors--;
	return (0);
}

int
zvol_remove_minor(const char *name)
{
	zvol_state_t *zv;
	int rc;

	mutex_enter(&zfsdev_state_lock);
	if ((zv = zvol_minor_lookup(name)) == NULL) {
		mutex_exit(&zfsdev_state_lock);
		return (SET_ERROR(ENXIO));
	}
	rc = zvol_remove_zv(zv);
	mutex_exit(&zfsdev_state_lock);
	return (rc);
}

int
zvol_first_open(zvol_state_t *zv)
{
	objset_t *os;
	uint64_t volsize;
	int error;
	uint64_t readonly;

	/* lie and say we're read-only */
	error = dmu_objset_own(zv->zv_name, DMU_OST_ZVOL, B_TRUE,
	    zvol_tag, &os);
	if (error)
		return (error);

	zv->zv_objset = os;
	error = zap_lookup(os, ZVOL_ZAP_OBJ, "size", 8, 1, &volsize);
	if (error) {
		ASSERT(error == 0);
		dmu_objset_disown(os, zvol_tag);
		return (error);
	}

	error = dmu_bonus_hold(os, ZVOL_OBJ, zvol_tag, &zv->zv_dbuf);
	if (error) {
		dmu_objset_disown(os, zvol_tag);
		return (error);
	}

	zvol_size_changed(zv, volsize);
	zv->zv_zilog = zil_open(os, zvol_get_data);

	VERIFY(dsl_prop_get_integer(zv->zv_name, "readonly", &readonly,
	    NULL) == 0);
	if (readonly || dmu_objset_is_snapshot(os) ||
	    !spa_writeable(dmu_objset_spa(os)))
		zv->zv_flags |= ZVOL_RDONLY;
	else
		zv->zv_flags &= ~ZVOL_RDONLY;
	return (error);
}

void
zvol_last_close(zvol_state_t *zv)
{
	zil_close(zv->zv_zilog);
	zv->zv_zilog = NULL;

	dmu_buf_rele(zv->zv_dbuf, zvol_tag);
	zv->zv_dbuf = NULL;

	/*
	 * Evict cached data
	 */
	if (dsl_dataset_is_dirty(dmu_objset_ds(zv->zv_objset)) &&
	    !(zv->zv_flags & ZVOL_RDONLY))
		txg_wait_synced(dmu_objset_pool(zv->zv_objset), 0);
	dmu_objset_evict_dbufs(zv->zv_objset);

	dmu_objset_disown(zv->zv_objset, zvol_tag);
	zv->zv_objset = NULL;
}

int
zvol_prealloc(zvol_state_t *zv)
{
	objset_t *os = zv->zv_objset;
	dmu_tx_t *tx;
	uint64_t refd, avail, usedobjs, availobjs;
	uint64_t resid = zv->zv_volsize;
	uint64_t off = 0;

	/* Check the space usage before attempting to allocate the space */
	dmu_objset_space(os, &refd, &avail, &usedobjs, &availobjs);
	if (avail < zv->zv_volsize)
		return (SET_ERROR(ENOSPC));

	/* Free old extents if they exist */
	zvol_free_extents(zv);

	while (resid != 0) {
		int error;
		uint64_t bytes = MIN(resid, SPA_MAXBLOCKSIZE);

		tx = dmu_tx_create(os);
		dmu_tx_hold_write(tx, ZVOL_OBJ, off, bytes);
		error = dmu_tx_assign(tx, TXG_WAIT);
		if (error) {
			dmu_tx_abort(tx);
			(void) dmu_free_long_range(os, ZVOL_OBJ, 0, off);
			return (error);
		}
		dmu_prealloc(os, ZVOL_OBJ, off, bytes, tx);
		dmu_tx_commit(tx);
		off += bytes;
		resid -= bytes;
	}
	txg_wait_synced(dmu_objset_pool(os), 0);

	return (0);
}

static int
zvol_update_volsize(objset_t *os, uint64_t volsize)
{
	dmu_tx_t *tx;
	int error;

	ASSERT(MUTEX_HELD(&zfsdev_state_lock));

	tx = dmu_tx_create(os);
	dmu_tx_hold_zap(tx, ZVOL_ZAP_OBJ, TRUE, NULL);
	error = dmu_tx_assign(tx, TXG_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		return (error);
	}

	error = zap_update(os, ZVOL_ZAP_OBJ, "size", 8, 1,
	    &volsize, tx);
	dmu_tx_commit(tx);

	if (error == 0)
		error = dmu_free_long_range(os,
		    ZVOL_OBJ, volsize, DMU_OBJECT_END);
	return (error);
}

void
zvol_remove_minors(const char *name)
{
	zvol_state_t *zv;
	char *namebuf;
	minor_t minor;

	namebuf = kmem_zalloc(strlen(name) + 2, KM_SLEEP);
	(void) strncpy(namebuf, name, strlen(name));
	(void) strcat(namebuf, "/");
	mutex_enter(&zfsdev_state_lock);
	for (minor = 1; minor <= ZFSDEV_MAX_MINOR; minor++) {

		zv = zfsdev_get_soft_state(minor, ZSST_ZVOL);
		if (zv == NULL)
			continue;
		if (strncmp(namebuf, zv->zv_name, strlen(namebuf)) == 0)
			(void) zvol_remove_zv(zv);
	}
	kmem_free(namebuf, strlen(name) + 2);

	mutex_exit(&zfsdev_state_lock);
}

static int
zvol_update_live_volsize(zvol_state_t *zv, uint64_t volsize)
{
	uint64_t old_volsize = 0ULL;
	int error = 0;

	ASSERT(MUTEX_HELD(&zfsdev_state_lock));

	/*
	 * Reinitialize the dump area to the new size. If we
	 * failed to resize the dump area then restore it back to
	 * its original size.  We must set the new volsize prior
	 * to calling dumpvp_resize() to ensure that the devices'
	 * size(9P) is not visible by the dump subsystem.
	 */
	old_volsize = zv->zv_volsize;
	zvol_size_changed(zv, volsize);

	if (zv->zv_flags & ZVOL_DUMPIFIED) {
		if ((error = zvol_dumpify(zv)) != 0 ||
		    (error = dumpvp_resize()) != 0) {
			int dumpify_error;

			(void) zvol_update_volsize(zv->zv_objset, old_volsize);
			zvol_size_changed(zv, old_volsize);
			dumpify_error = zvol_dumpify(zv);
			error = dumpify_error ? dumpify_error : error;
		}
	}

	/*
	 * Generate a LUN expansion event.
	 */
	if (error == 0) {
		sysevent_id_t eid;
		nvlist_t *attr;
		char *physpath = kmem_zalloc(MAXPATHLEN, KM_SLEEP);

		(void) snprintf(physpath, MAXPATHLEN, "%s%u", ZVOL_PSEUDO_DEV,
		    zv->zv_minor);

		VERIFY(nvlist_alloc(&attr, NV_UNIQUE_NAME, KM_SLEEP) == 0);
		VERIFY(nvlist_add_string(attr, DEV_PHYS_PATH, physpath) == 0);

		(void) ddi_log_sysevent(zfs_dip, SUNW_VENDOR, EC_DEV_STATUS,
		    ESC_DEV_DLE, attr, &eid, DDI_SLEEP);

		nvlist_free(attr);
		kmem_free(physpath, MAXPATHLEN);
	}
	return (error);
}

int
zvol_set_volsize(const char *name, uint64_t volsize)
{
	zvol_state_t *zv = NULL;
	objset_t *os;
	int error;
	dmu_object_info_t doi;
	uint64_t readonly;
	boolean_t owned = B_FALSE;

	error = dsl_prop_get_integer(name,
	    zfs_prop_to_name(ZFS_PROP_READONLY), &readonly, NULL);
	if (error != 0)
		return (error);
	if (readonly)
		return (SET_ERROR(EROFS));

	mutex_enter(&zfsdev_state_lock);
	zv = zvol_minor_lookup(name);

	if (zv == NULL || zv->zv_objset == NULL) {
		if ((error = dmu_objset_own(name, DMU_OST_ZVOL, B_FALSE,
		    FTAG, &os)) != 0) {
			mutex_exit(&zfsdev_state_lock);
			return (error);
		}
		owned = B_TRUE;
		if (zv != NULL)
			zv->zv_objset = os;
	} else {
		os = zv->zv_objset;
	}

	if ((error = dmu_object_info(os, ZVOL_OBJ, &doi)) != 0 ||
	    (error = zvol_check_volsize(volsize, doi.doi_data_block_size)) != 0)
		goto out;

	error = zvol_update_volsize(os, volsize);

	if (error == 0 && zv != NULL)
		error = zvol_update_live_volsize(zv, volsize);
out:
	if (owned) {
		dmu_objset_disown(os, FTAG);
		if (zv != NULL)
			zv->zv_objset = NULL;
	}
	mutex_exit(&zfsdev_state_lock);
	return (error);
}

/*ARGSUSED*/
int
zvol_open(dev_t *devp, int flag, int otyp, cred_t *cr)
{
	zvol_state_t *zv;
	int err = 0;

	mutex_enter(&zfsdev_state_lock);

	zv = zfsdev_get_soft_state(getminor(*devp), ZSST_ZVOL);
	if (zv == NULL) {
		mutex_exit(&zfsdev_state_lock);
		return (SET_ERROR(ENXIO));
	}

	if (zv->zv_total_opens == 0)
		err = zvol_first_open(zv);
	if (err) {
		mutex_exit(&zfsdev_state_lock);
		return (err);
	}
	if ((flag & FWRITE) && (zv->zv_flags & ZVOL_RDONLY)) {
		err = SET_ERROR(EROFS);
		goto out;
	}
	if (zv->zv_flags & ZVOL_EXCL) {
		err = SET_ERROR(EBUSY);
		goto out;
	}
	if (flag & FEXCL) {
		if (zv->zv_total_opens != 0) {
			err = SET_ERROR(EBUSY);
			goto out;
		}
		zv->zv_flags |= ZVOL_EXCL;
	}

	if (zv->zv_open_count[otyp] == 0 || otyp == OTYP_LYR) {
		zv->zv_open_count[otyp]++;
		zv->zv_total_opens++;
	}
	mutex_exit(&zfsdev_state_lock);

	return (err);
out:
	if (zv->zv_total_opens == 0)
		zvol_last_close(zv);
	mutex_exit(&zfsdev_state_lock);
	return (err);
}

/*ARGSUSED*/
int
zvol_close(dev_t dev, int flag, int otyp, cred_t *cr)
{
	minor_t minor = getminor(dev);
	zvol_state_t *zv;
	int error = 0;

	mutex_enter(&zfsdev_state_lock);

	zv = zfsdev_get_soft_state(minor, ZSST_ZVOL);
	if (zv == NULL) {
		mutex_exit(&zfsdev_state_lock);
		return (SET_ERROR(ENXIO));
	}

	if (zv->zv_flags & ZVOL_EXCL) {
		ASSERT(zv->zv_total_opens == 1);
		zv->zv_flags &= ~ZVOL_EXCL;
	}

	/*
	 * If the open count is zero, this is a spurious close.
	 * That indicates a bug in the kernel / DDI framework.
	 */
	ASSERT(zv->zv_open_count[otyp] != 0);
	ASSERT(zv->zv_total_opens != 0);

	/*
	 * You may get multiple opens, but only one close.
	 */
	zv->zv_open_count[otyp]--;
	zv->zv_total_opens--;

	if (zv->zv_total_opens == 0)
		zvol_last_close(zv);

	mutex_exit(&zfsdev_state_lock);
	return (error);
}

static void
zvol_get_done(zgd_t *zgd, int error)
{
	if (zgd->zgd_db)
		dmu_buf_rele(zgd->zgd_db, zgd);

	zfs_range_unlock(zgd->zgd_rl);

	if (error == 0 && zgd->zgd_bp)
		zil_add_block(zgd->zgd_zilog, zgd->zgd_bp);

	kmem_free(zgd, sizeof (zgd_t));
}

/*
 * Get data to generate a TX_WRITE intent log record.
 */
static int
zvol_get_data(void *arg, lr_write_t *lr, char *buf, zio_t *zio)
{
	zvol_state_t *zv = arg;
	objset_t *os = zv->zv_objset;
	uint64_t object = ZVOL_OBJ;
	uint64_t offset = lr->lr_offset;
	uint64_t size = lr->lr_length;	/* length of user data */
	blkptr_t *bp = &lr->lr_blkptr;
	dmu_buf_t *db;
	zgd_t *zgd;
	int error;

	ASSERT(zio != NULL);
	ASSERT(size != 0);

	zgd = kmem_zalloc(sizeof (zgd_t), KM_SLEEP);
	zgd->zgd_zilog = zv->zv_zilog;
	zgd->zgd_rl = zfs_range_lock(&zv->zv_znode, offset, size, RL_READER);

	/*
	 * Write records come in two flavors: immediate and indirect.
	 * For small writes it's cheaper to store the data with the
	 * log record (immediate); for large writes it's cheaper to
	 * sync the data and get a pointer to it (indirect) so that
	 * we don't have to write the data twice.
	 */
	if (buf != NULL) {	/* immediate write */
		error = dmu_read(os, object, offset, size, buf,
		    DMU_READ_NO_PREFETCH);
	} else {
		size = zv->zv_volblocksize;
		offset = P2ALIGN(offset, size);
		error = dmu_buf_hold(os, object, offset, zgd, &db,
		    DMU_READ_NO_PREFETCH);
		if (error == 0) {
			blkptr_t *obp = dmu_buf_get_blkptr(db);
			if (obp) {
				ASSERT(BP_IS_HOLE(bp));
				*bp = *obp;
			}

			zgd->zgd_db = db;
			zgd->zgd_bp = bp;

			ASSERT(db->db_offset == offset);
			ASSERT(db->db_size == size);

			error = dmu_sync(zio, lr->lr_common.lrc_txg,
			    zvol_get_done, zgd);

			if (error == 0)
				return (0);
		}
	}

	zvol_get_done(zgd, error);

	return (error);
}

/*
 * zvol_log_write() handles synchronous writes using TX_WRITE ZIL transactions.
 *
 * We store data in the log buffers if it's small enough.
 * Otherwise we will later flush the data out via dmu_sync().
 */
ssize_t zvol_immediate_write_sz = 32768;

static void
zvol_log_write(zvol_state_t *zv, dmu_tx_t *tx, offset_t off, ssize_t resid,
    boolean_t sync)
{
	uint32_t blocksize = zv->zv_volblocksize;
	zilog_t *zilog = zv->zv_zilog;
	boolean_t slogging;
	ssize_t immediate_write_sz;

	if (zil_replaying(zilog, tx))
		return;

	immediate_write_sz = (zilog->zl_logbias == ZFS_LOGBIAS_THROUGHPUT)
	    ? 0 : zvol_immediate_write_sz;

	slogging = spa_has_slogs(zilog->zl_spa) &&
	    (zilog->zl_logbias == ZFS_LOGBIAS_LATENCY);

	while (resid) {
		itx_t *itx;
		lr_write_t *lr;
		ssize_t len;
		itx_wr_state_t write_state;

		/*
		 * Unlike zfs_log_write() we can be called with
		 * upto DMU_MAX_ACCESS/2 (5MB) writes.
		 */
		if (blocksize > immediate_write_sz && !slogging &&
		    resid >= blocksize && off % blocksize == 0) {
			write_state = WR_INDIRECT; /* uses dmu_sync */
			len = blocksize;
		} else if (sync) {
			write_state = WR_COPIED;
			len = MIN(ZIL_MAX_LOG_DATA, resid);
		} else {
			write_state = WR_NEED_COPY;
			len = MIN(ZIL_MAX_LOG_DATA, resid);
		}

		itx = zil_itx_create(TX_WRITE, sizeof (*lr) +
		    (write_state == WR_COPIED ? len : 0));
		lr = (lr_write_t *)&itx->itx_lr;
		if (write_state == WR_COPIED && dmu_read(zv->zv_objset,
		    ZVOL_OBJ, off, len, lr + 1, DMU_READ_NO_PREFETCH) != 0) {
			zil_itx_destroy(itx);
			itx = zil_itx_create(TX_WRITE, sizeof (*lr));
			lr = (lr_write_t *)&itx->itx_lr;
			write_state = WR_NEED_COPY;
		}

		itx->itx_wr_state = write_state;
		if (write_state == WR_NEED_COPY)
			itx->itx_sod += len;
		lr->lr_foid = ZVOL_OBJ;
		lr->lr_offset = off;
		lr->lr_length = len;
		lr->lr_blkoff = 0;
		BP_ZERO(&lr->lr_blkptr);

		itx->itx_private = zv;
		itx->itx_sync = sync;

		zil_itx_assign(zilog, itx, tx);

		off += len;
		resid -= len;
	}
}

static int
zvol_dumpio_vdev(vdev_t *vd, void *addr, uint64_t offset, uint64_t origoffset,
    uint64_t size, boolean_t doread, boolean_t isdump)
{
	vdev_disk_t *dvd;
	int c;
	int numerrors = 0;

	if (vd->vdev_ops == &vdev_mirror_ops ||
	    vd->vdev_ops == &vdev_replacing_ops ||
	    vd->vdev_ops == &vdev_spare_ops) {
		for (c = 0; c < vd->vdev_children; c++) {
			int err = zvol_dumpio_vdev(vd->vdev_child[c],
			    addr, offset, origoffset, size, doread, isdump);
			if (err != 0) {
				numerrors++;
			} else if (doread) {
				break;
			}
		}
	}

	if (!vd->vdev_ops->vdev_op_leaf && vd->vdev_ops != &vdev_raidz_ops)
		return (numerrors < vd->vdev_children ? 0 : EIO);

	if (doread && !vdev_readable(vd))
		return (SET_ERROR(EIO));
	else if (!doread && !vdev_writeable(vd))
		return (SET_ERROR(EIO));

	if (vd->vdev_ops == &vdev_raidz_ops) {
		return (vdev_raidz_physio(vd,
		    addr, size, offset, origoffset, doread, isdump));
	}

	offset += VDEV_LABEL_START_SIZE;

	if (ddi_in_panic() || isdump) {
		ASSERT(!doread);
		if (doread)
			return (SET_ERROR(EIO));
		dvd = vd->vdev_tsd;
		ASSERT3P(dvd, !=, NULL);
		return (ldi_dump(dvd->vd_lh, addr, lbtodb(offset),
		    lbtodb(size)));
	} else {
		dvd = vd->vdev_tsd;
		ASSERT3P(dvd, !=, NULL);
		return (vdev_disk_ldi_physio(dvd->vd_lh, addr, size,
		    offset, doread ? B_READ : B_WRITE));
	}
}

static int
zvol_dumpio(zvol_state_t *zv, void *addr, uint64_t offset, uint64_t size,
    boolean_t doread, boolean_t isdump)
{
	vdev_t *vd;
	int error;
	zvol_extent_t *ze;
	spa_t *spa = dmu_objset_spa(zv->zv_objset);

	/* Must be sector aligned, and not stradle a block boundary. */
	if (P2PHASE(offset, DEV_BSIZE) || P2PHASE(size, DEV_BSIZE) ||
	    P2BOUNDARY(offset, size, zv->zv_volblocksize)) {
		return (SET_ERROR(EINVAL));
	}
	ASSERT(size <= zv->zv_volblocksize);

	/* Locate the extent this belongs to */
	ze = list_head(&zv->zv_extents);
	while (offset >= ze->ze_nblks * zv->zv_volblocksize) {
		offset -= ze->ze_nblks * zv->zv_volblocksize;
		ze = list_next(&zv->zv_extents, ze);
	}

	if (ze == NULL)
		return (SET_ERROR(EINVAL));

	if (!ddi_in_panic())
		spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);

	vd = vdev_lookup_top(spa, DVA_GET_VDEV(&ze->ze_dva));
	offset += DVA_GET_OFFSET(&ze->ze_dva);
	error = zvol_dumpio_vdev(vd, addr, offset, DVA_GET_OFFSET(&ze->ze_dva),
	    size, doread, isdump);

	if (!ddi_in_panic())
		spa_config_exit(spa, SCL_STATE, FTAG);

	return (error);
}

int
zvol_strategy(buf_t *bp)
{
	zfs_soft_state_t *zs = NULL;
	zvol_state_t *zv;
	uint64_t off, volsize;
	size_t resid;
	char *addr;
	objset_t *os;
	rl_t *rl;
	int error = 0;
	boolean_t doread = bp->b_flags & B_READ;
	boolean_t is_dumpified;
	boolean_t sync;

	if (getminor(bp->b_edev) == 0) {
		error = SET_ERROR(EINVAL);
	} else {
		zs = ddi_get_soft_state(zfsdev_state, getminor(bp->b_edev));
		if (zs == NULL)
			error = SET_ERROR(ENXIO);
		else if (zs->zss_type != ZSST_ZVOL)
			error = SET_ERROR(EINVAL);
	}

	if (error) {
		bioerror(bp, error);
		biodone(bp);
		return (0);
	}

	zv = zs->zss_data;

	if (!(bp->b_flags & B_READ) && (zv->zv_flags & ZVOL_RDONLY)) {
		bioerror(bp, EROFS);
		biodone(bp);
		return (0);
	}

	off = ldbtob(bp->b_blkno);
	volsize = zv->zv_volsize;

	os = zv->zv_objset;
	ASSERT(os != NULL);

	bp_mapin(bp);
	addr = bp->b_un.b_addr;
	resid = bp->b_bcount;

	if (resid > 0 && (off < 0 || off >= volsize)) {
		bioerror(bp, EIO);
		biodone(bp);
		return (0);
	}

	is_dumpified = zv->zv_flags & ZVOL_DUMPIFIED;
	sync = ((!(bp->b_flags & B_ASYNC) &&
	    !(zv->zv_flags & ZVOL_WCE)) ||
	    (zv->zv_objset->os_sync == ZFS_SYNC_ALWAYS)) &&
	    !doread && !is_dumpified;

	/*
	 * There must be no buffer changes when doing a dmu_sync() because
	 * we can't change the data whilst calculating the checksum.
	 */
	rl = zfs_range_lock(&zv->zv_znode, off, resid,
	    doread ? RL_READER : RL_WRITER);

	while (resid != 0 && off < volsize) {
		size_t size = MIN(resid, zvol_maxphys);
		if (is_dumpified) {
			size = MIN(size, P2END(off, zv->zv_volblocksize) - off);
			error = zvol_dumpio(zv, addr, off, size,
			    doread, B_FALSE);
		} else if (doread) {
			error = dmu_read(os, ZVOL_OBJ, off, size, addr,
			    DMU_READ_PREFETCH);
		} else {
			dmu_tx_t *tx = dmu_tx_create(os);
			dmu_tx_hold_write(tx, ZVOL_OBJ, off, size);
			error = dmu_tx_assign(tx, TXG_WAIT);
			if (error) {
				dmu_tx_abort(tx);
			} else {
				dmu_write(os, ZVOL_OBJ, off, size, addr, tx);
				zvol_log_write(zv, tx, off, size, sync);
				dmu_tx_commit(tx);
			}
		}
		if (error) {
			/* convert checksum errors into IO errors */
			if (error == ECKSUM)
				error = SET_ERROR(EIO);
			break;
		}
		off += size;
		addr += size;
		resid -= size;
	}
	zfs_range_unlock(rl);

	if ((bp->b_resid = resid) == bp->b_bcount)
		bioerror(bp, off > volsize ? EINVAL : error);

	if (sync)
		zil_commit(zv->zv_zilog, ZVOL_OBJ);
	biodone(bp);

	return (0);
}

/*
 * Set the buffer count to the zvol maximum transfer.
 * Using our own routine instead of the default minphys()
 * means that for larger writes we write bigger buffers on X86
 * (128K instead of 56K) and flush the disk write cache less often
 * (every zvol_maxphys - currently 1MB) instead of minphys (currently
 * 56K on X86 and 128K on sparc).
 */
void
zvol_minphys(struct buf *bp)
{
	if (bp->b_bcount > zvol_maxphys)
		bp->b_bcount = zvol_maxphys;
}

int
zvol_dump(dev_t dev, caddr_t addr, daddr_t blkno, int nblocks)
{
	minor_t minor = getminor(dev);
	zvol_state_t *zv;
	int error = 0;
	uint64_t size;
	uint64_t boff;
	uint64_t resid;

	zv = zfsdev_get_soft_state(minor, ZSST_ZVOL);
	if (zv == NULL)
		return (SET_ERROR(ENXIO));

	if ((zv->zv_flags & ZVOL_DUMPIFIED) == 0)
		return (SET_ERROR(EINVAL));

	boff = ldbtob(blkno);
	resid = ldbtob(nblocks);

	VERIFY3U(boff + resid, <=, zv->zv_volsize);

	while (resid) {
		size = MIN(resid, P2END(boff, zv->zv_volblocksize) - boff);
		error = zvol_dumpio(zv, addr, boff, size, B_FALSE, B_TRUE);
		if (error)
			break;
		boff += size;
		addr += size;
		resid -= size;
	}

	return (error);
}

/*ARGSUSED*/
int
zvol_read(dev_t dev, uio_t *uio, cred_t *cr)
{
	minor_t minor = getminor(dev);
	zvol_state_t *zv;
	uint64_t volsize;
	rl_t *rl;
	int error = 0;

	zv = zfsdev_get_soft_state(minor, ZSST_ZVOL);
	if (zv == NULL)
		return (SET_ERROR(ENXIO));

	volsize = zv->zv_volsize;
	if (uio->uio_resid > 0 &&
	    (uio->uio_loffset < 0 || uio->uio_loffset >= volsize))
		return (SET_ERROR(EIO));

	if (zv->zv_flags & ZVOL_DUMPIFIED) {
		error = physio(zvol_strategy, NULL, dev, B_READ,
		    zvol_minphys, uio);
		return (error);
	}

	DTRACE_PROBE3(zvol__uio__start, dev_t, dev, uio_t *, uio, int, 0);

	rl = zfs_range_lock(&zv->zv_znode, uio->uio_loffset, uio->uio_resid,
	    RL_READER);
	while (uio->uio_resid > 0 && uio->uio_loffset < volsize) {
		uint64_t bytes = MIN(uio->uio_resid, DMU_MAX_ACCESS >> 1);

		/* don't read past the end */
		if (bytes > volsize - uio->uio_loffset)
			bytes = volsize - uio->uio_loffset;

		error =  dmu_read_uio(zv->zv_objset, ZVOL_OBJ, uio, bytes);
		if (error) {
			/* convert checksum errors into IO errors */
			if (error == ECKSUM)
				error = SET_ERROR(EIO);
			break;
		}
	}
	zfs_range_unlock(rl);

	DTRACE_PROBE4(zvol__uio__done, dev_t, dev, uio_t *, uio, int, 0, int,
	    error);

	return (error);
}

/*ARGSUSED*/
int
zvol_write(dev_t dev, uio_t *uio, cred_t *cr)
{
	minor_t minor = getminor(dev);
	zvol_state_t *zv;
	uint64_t volsize;
	rl_t *rl;
	int error = 0;
	boolean_t sync;

	zv = zfsdev_get_soft_state(minor, ZSST_ZVOL);
	if (zv == NULL)
		return (SET_ERROR(ENXIO));

	volsize = zv->zv_volsize;
	if (uio->uio_resid > 0 &&
	    (uio->uio_loffset < 0 || uio->uio_loffset >= volsize))
		return (SET_ERROR(EIO));

	if (zv->zv_flags & ZVOL_DUMPIFIED) {
		error = physio(zvol_strategy, NULL, dev, B_WRITE,
		    zvol_minphys, uio);
		return (error);
	}

	DTRACE_PROBE3(zvol__uio__start, dev_t, dev, uio_t *, uio, int, 1);

	sync = !(zv->zv_flags & ZVOL_WCE) ||
	    (zv->zv_objset->os_sync == ZFS_SYNC_ALWAYS);

	rl = zfs_range_lock(&zv->zv_znode, uio->uio_loffset, uio->uio_resid,
	    RL_WRITER);
	while (uio->uio_resid > 0 && uio->uio_loffset < volsize) {
		uint64_t bytes = MIN(uio->uio_resid, DMU_MAX_ACCESS >> 1);
		uint64_t off = uio->uio_loffset;
		dmu_tx_t *tx = dmu_tx_create(zv->zv_objset);

		if (bytes > volsize - off)	/* don't write past the end */
			bytes = volsize - off;

		dmu_tx_hold_write(tx, ZVOL_OBJ, off, bytes);
		error = dmu_tx_assign(tx, TXG_WAIT);
		if (error) {
			dmu_tx_abort(tx);
			break;
		}
		error = dmu_write_uio_dbuf(zv->zv_dbuf, uio, bytes, tx);
		if (error == 0)
			zvol_log_write(zv, tx, off, bytes, sync);
		dmu_tx_commit(tx);

		if (error)
			break;
	}
	zfs_range_unlock(rl);
	if (sync)
		zil_commit(zv->zv_zilog, ZVOL_OBJ);

	DTRACE_PROBE4(zvol__uio__done, dev_t, dev, uio_t *, uio, int, 1, int,
	    error);

	return (error);
}

int
zvol_getefi(void *arg, int flag, uint64_t vs, uint8_t bs)
{
	struct uuid uuid = EFI_RESERVED;
	efi_gpe_t gpe = { 0 };
	uint32_t crc;
	dk_efi_t efi;
	int length;
	char *ptr;

	if (ddi_copyin(arg, &efi, sizeof (dk_efi_t), flag))
		return (SET_ERROR(EFAULT));
	ptr = (char *)(uintptr_t)efi.dki_data_64;
	length = efi.dki_length;
	/*
	 * Some clients may attempt to request a PMBR for the
	 * zvol.  Currently this interface will return EINVAL to
	 * such requests.  These requests could be supported by
	 * adding a check for lba == 0 and consing up an appropriate
	 * PMBR.
	 */
	if (efi.dki_lba < 1 || efi.dki_lba > 2 || length <= 0)
		return (SET_ERROR(EINVAL));

	gpe.efi_gpe_StartingLBA = LE_64(34ULL);
	gpe.efi_gpe_EndingLBA = LE_64((vs >> bs) - 1);
	UUID_LE_CONVERT(gpe.efi_gpe_PartitionTypeGUID, uuid);

	if (efi.dki_lba == 1) {
		efi_gpt_t gpt = { 0 };

		gpt.efi_gpt_Signature = LE_64(EFI_SIGNATURE);
		gpt.efi_gpt_Revision = LE_32(EFI_VERSION_CURRENT);
		gpt.efi_gpt_HeaderSize = LE_32(sizeof (gpt));
		gpt.efi_gpt_MyLBA = LE_64(1ULL);
		gpt.efi_gpt_FirstUsableLBA = LE_64(34ULL);
		gpt.efi_gpt_LastUsableLBA = LE_64((vs >> bs) - 1);
		gpt.efi_gpt_PartitionEntryLBA = LE_64(2ULL);
		gpt.efi_gpt_NumberOfPartitionEntries = LE_32(1);
		gpt.efi_gpt_SizeOfPartitionEntry =
		    LE_32(sizeof (efi_gpe_t));
		CRC32(crc, &gpe, sizeof (gpe), -1U, crc32_table);
		gpt.efi_gpt_PartitionEntryArrayCRC32 = LE_32(~crc);
		CRC32(crc, &gpt, sizeof (gpt), -1U, crc32_table);
		gpt.efi_gpt_HeaderCRC32 = LE_32(~crc);
		if (ddi_copyout(&gpt, ptr, MIN(sizeof (gpt), length),
		    flag))
			return (SET_ERROR(EFAULT));
		ptr += sizeof (gpt);
		length -= sizeof (gpt);
	}
	if (length > 0 && ddi_copyout(&gpe, ptr, MIN(sizeof (gpe),
	    length), flag))
		return (SET_ERROR(EFAULT));
	return (0);
}

/*
 * BEGIN entry points to allow external callers access to the volume.
 */
/*
 * Return the volume parameters needed for access from an external caller.
 * These values are invariant as long as the volume is held open.
 */
int
zvol_get_volume_params(minor_t minor, uint64_t *blksize,
    uint64_t *max_xfer_len, void **minor_hdl, void **objset_hdl, void **zil_hdl,
    void **rl_hdl, void **bonus_hdl)
{
	zvol_state_t *zv;

	zv = zfsdev_get_soft_state(minor, ZSST_ZVOL);
	if (zv == NULL)
		return (SET_ERROR(ENXIO));
	if (zv->zv_flags & ZVOL_DUMPIFIED)
		return (SET_ERROR(ENXIO));

	ASSERT(blksize && max_xfer_len && minor_hdl &&
	    objset_hdl && zil_hdl && rl_hdl && bonus_hdl);

	*blksize = zv->zv_volblocksize;
	*max_xfer_len = (uint64_t)zvol_maxphys;
	*minor_hdl = zv;
	*objset_hdl = zv->zv_objset;
	*zil_hdl = zv->zv_zilog;
	*rl_hdl = &zv->zv_znode;
	*bonus_hdl = zv->zv_dbuf;
	return (0);
}

/*
 * Return the current volume size to an external caller.
 * The size can change while the volume is open.
 */
uint64_t
zvol_get_volume_size(void *minor_hdl)
{
	zvol_state_t *zv = minor_hdl;

	return (zv->zv_volsize);
}

/*
 * Return the current WCE setting to an external caller.
 * The WCE setting can change while the volume is open.
 */
int
zvol_get_volume_wce(void *minor_hdl)
{
	zvol_state_t *zv = minor_hdl;

	return ((zv->zv_flags & ZVOL_WCE) ? 1 : 0);
}

/*
 * Entry point for external callers to zvol_log_write
 */
void
zvol_log_write_minor(void *minor_hdl, dmu_tx_t *tx, offset_t off, ssize_t resid,
    boolean_t sync)
{
	zvol_state_t *zv = minor_hdl;

	zvol_log_write(zv, tx, off, resid, sync);
}
/*
 * END entry points to allow external callers access to the volume.
 */

/*
 * Log a DKIOCFREE/free-long-range to the ZIL with TX_TRUNCATE.
 */
static void
zvol_log_truncate(zvol_state_t *zv, dmu_tx_t *tx, uint64_t off, uint64_t len,
    boolean_t sync)
{
	itx_t *itx;
	lr_truncate_t *lr;
	zilog_t *zilog = zv->zv_zilog;

	if (zil_replaying(zilog, tx))
		return;

	itx = zil_itx_create(TX_TRUNCATE, sizeof (*lr));
	lr = (lr_truncate_t *)&itx->itx_lr;
	lr->lr_foid = ZVOL_OBJ;
	lr->lr_offset = off;
	lr->lr_length = len;

	itx->itx_sync = sync;
	zil_itx_assign(zilog, itx, tx);
}

/*
 * Dirtbag ioctls to support mkfs(1M) for UFS filesystems.  See dkio(7I).
 * Also a dirtbag dkio ioctl for unmap/free-block functionality.
 */
/*ARGSUSED*/
int
zvol_ioctl(dev_t dev, int cmd, intptr_t arg, int flag, cred_t *cr, int *rvalp)
{
	zvol_state_t *zv;
	struct dk_cinfo dki;
	struct dk_minfo dkm;
	struct dk_callback *dkc;
	int error = 0;
	rl_t *rl;

	mutex_enter(&zfsdev_state_lock);

	zv = zfsdev_get_soft_state(getminor(dev), ZSST_ZVOL);

	if (zv == NULL) {
		mutex_exit(&zfsdev_state_lock);
		return (SET_ERROR(ENXIO));
	}
	ASSERT(zv->zv_total_opens > 0);

	switch (cmd) {

	case DKIOCINFO:
		bzero(&dki, sizeof (dki));
		(void) strcpy(dki.dki_cname, "zvol");
		(void) strcpy(dki.dki_dname, "zvol");
		dki.dki_ctype = DKC_UNKNOWN;
		dki.dki_unit = getminor(dev);
		dki.dki_maxtransfer = 1 << (SPA_MAXBLOCKSHIFT - zv->zv_min_bs);
		mutex_exit(&zfsdev_state_lock);
		if (ddi_copyout(&dki, (void *)arg, sizeof (dki), flag))
			error = SET_ERROR(EFAULT);
		return (error);

	case DKIOCGMEDIAINFO:
		bzero(&dkm, sizeof (dkm));
		dkm.dki_lbsize = 1U << zv->zv_min_bs;
		dkm.dki_capacity = zv->zv_volsize >> zv->zv_min_bs;
		dkm.dki_media_type = DK_UNKNOWN;
		mutex_exit(&zfsdev_state_lock);
		if (ddi_copyout(&dkm, (void *)arg, sizeof (dkm), flag))
			error = SET_ERROR(EFAULT);
		return (error);

	case DKIOCGETEFI:
		{
			uint64_t vs = zv->zv_volsize;
			uint8_t bs = zv->zv_min_bs;

			mutex_exit(&zfsdev_state_lock);
			error = zvol_getefi((void *)arg, flag, vs, bs);
			return (error);
		}

	case DKIOCFLUSHWRITECACHE:
		dkc = (struct dk_callback *)arg;
		mutex_exit(&zfsdev_state_lock);
		zil_commit(zv->zv_zilog, ZVOL_OBJ);
		if ((flag & FKIOCTL) && dkc != NULL && dkc->dkc_callback) {
			(*dkc->dkc_callback)(dkc->dkc_cookie, error);
			error = 0;
		}
		return (error);

	case DKIOCGETWCE:
		{
			int wce = (zv->zv_flags & ZVOL_WCE) ? 1 : 0;
			if (ddi_copyout(&wce, (void *)arg, sizeof (int),
			    flag))
				error = SET_ERROR(EFAULT);
			break;
		}
	case DKIOCSETWCE:
		{
			int wce;
			if (ddi_copyin((void *)arg, &wce, sizeof (int),
			    flag)) {
				error = SET_ERROR(EFAULT);
				break;
			}
			if (wce) {
				zv->zv_flags |= ZVOL_WCE;
				mutex_exit(&zfsdev_state_lock);
			} else {
				zv->zv_flags &= ~ZVOL_WCE;
				mutex_exit(&zfsdev_state_lock);
				zil_commit(zv->zv_zilog, ZVOL_OBJ);
			}
			return (0);
		}

	case DKIOCGGEOM:
	case DKIOCGVTOC:
		/*
		 * commands using these (like prtvtoc) expect ENOTSUP
		 * since we're emulating an EFI label
		 */
		error = SET_ERROR(ENOTSUP);
		break;

	case DKIOCDUMPINIT:
		rl = zfs_range_lock(&zv->zv_znode, 0, zv->zv_volsize,
		    RL_WRITER);
		error = zvol_dumpify(zv);
		zfs_range_unlock(rl);
		break;

	case DKIOCDUMPFINI:
		if (!(zv->zv_flags & ZVOL_DUMPIFIED))
			break;
		rl = zfs_range_lock(&zv->zv_znode, 0, zv->zv_volsize,
		    RL_WRITER);
		error = zvol_dump_fini(zv);
		zfs_range_unlock(rl);
		break;

	case DKIOCFREE:
	{
		dkioc_free_t df;
		dmu_tx_t *tx;

		if (ddi_copyin((void *)arg, &df, sizeof (df), flag)) {
			error = SET_ERROR(EFAULT);
			break;
		}

		/*
		 * Apply Postel's Law to length-checking.  If they overshoot,
		 * just blank out until the end, if there's a need to blank
		 * out anything.
		 */
		if (df.df_start >= zv->zv_volsize)
			break;	/* No need to do anything... */
		if (df.df_start + df.df_length > zv->zv_volsize)
			df.df_length = DMU_OBJECT_END;

		rl = zfs_range_lock(&zv->zv_znode, df.df_start, df.df_length,
		    RL_WRITER);
		tx = dmu_tx_create(zv->zv_objset);
		error = dmu_tx_assign(tx, TXG_WAIT);
		if (error != 0) {
			dmu_tx_abort(tx);
		} else {
			zvol_log_truncate(zv, tx, df.df_start,
			    df.df_length, B_TRUE);
			dmu_tx_commit(tx);
			error = dmu_free_long_range(zv->zv_objset, ZVOL_OBJ,
			    df.df_start, df.df_length);
		}

		zfs_range_unlock(rl);

		if (error == 0) {
			/*
			 * If the write-cache is disabled or 'sync' property
			 * is set to 'always' then treat this as a synchronous
			 * operation (i.e. commit to zil).
			 */
			if (!(zv->zv_flags & ZVOL_WCE) ||
			    (zv->zv_objset->os_sync == ZFS_SYNC_ALWAYS))
				zil_commit(zv->zv_zilog, ZVOL_OBJ);

			/*
			 * If the caller really wants synchronous writes, and
			 * can't wait for them, don't return until the write
			 * is done.
			 */
			if (df.df_flags & DF_WAIT_SYNC) {
				txg_wait_synced(
				    dmu_objset_pool(zv->zv_objset), 0);
			}
		}
		break;
	}

	default:
		error = SET_ERROR(ENOTTY);
		break;

	}
	mutex_exit(&zfsdev_state_lock);
	return (error);
}

int
zvol_busy(void)
{
	return (zvol_minors != 0);
}

void
zvol_init(void)
{
	VERIFY(ddi_soft_state_init(&zfsdev_state, sizeof (zfs_soft_state_t),
	    1) == 0);
	mutex_init(&zfsdev_state_lock, NULL, MUTEX_DEFAULT, NULL);
}

void
zvol_fini(void)
{
	mutex_destroy(&zfsdev_state_lock);
	ddi_soft_state_fini(&zfsdev_state);
}

/*ARGSUSED*/
static int
zfs_mvdev_dump_feature_check(void *arg, dmu_tx_t *tx)
{
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;

	if (spa_feature_is_active(spa,
	    &spa_feature_table[SPA_FEATURE_MULTI_VDEV_CRASH_DUMP]))
		return (1);
	return (0);
}

/*ARGSUSED*/
static void
zfs_mvdev_dump_activate_feature_sync(void *arg, dmu_tx_t *tx)
{
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;

	spa_feature_incr(spa,
	    &spa_feature_table[SPA_FEATURE_MULTI_VDEV_CRASH_DUMP], tx);
}

static int
zvol_dump_init(zvol_state_t *zv, boolean_t resize)
{
	dmu_tx_t *tx;
	int error;
	objset_t *os = zv->zv_objset;
	spa_t *spa = dmu_objset_spa(os);
	vdev_t *vd = spa->spa_root_vdev;
	nvlist_t *nv = NULL;
	uint64_t version = spa_version(spa);
	enum zio_checksum checksum;

	ASSERT(MUTEX_HELD(&zfsdev_state_lock));
	ASSERT(vd->vdev_ops == &vdev_root_ops);

	error = dmu_free_long_range(zv->zv_objset, ZVOL_OBJ, 0,
	    DMU_OBJECT_END);
	/* wait for dmu_free_long_range to actually free the blocks */
	txg_wait_synced(dmu_objset_pool(zv->zv_objset), 0);

	/*
	 * If the pool on which the dump device is being initialized has more
	 * than one child vdev, check that the MULTI_VDEV_CRASH_DUMP feature is
	 * enabled.  If so, bump that feature's counter to indicate that the
	 * feature is active. We also check the vdev type to handle the
	 * following case:
	 *   # zpool create test raidz disk1 disk2 disk3
	 *   Now have spa_root_vdev->vdev_children == 1 (the raidz vdev),
	 *   the raidz vdev itself has 3 children.
	 */
	if (vd->vdev_children > 1 || vd->vdev_ops == &vdev_raidz_ops) {
		if (!spa_feature_is_enabled(spa,
		    &spa_feature_table[SPA_FEATURE_MULTI_VDEV_CRASH_DUMP]))
			return (SET_ERROR(ENOTSUP));
		(void) dsl_sync_task(spa_name(spa),
		    zfs_mvdev_dump_feature_check,
		    zfs_mvdev_dump_activate_feature_sync, NULL, 2);
	}

	tx = dmu_tx_create(os);
	dmu_tx_hold_zap(tx, ZVOL_ZAP_OBJ, TRUE, NULL);
	dmu_tx_hold_bonus(tx, ZVOL_OBJ);
	error = dmu_tx_assign(tx, TXG_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		return (error);
	}

	/*
	 * If MULTI_VDEV_CRASH_DUMP is active, use the NOPARITY checksum
	 * function.  Otherwise, use the old default -- OFF.
	 */
	checksum = spa_feature_is_active(spa,
	    &spa_feature_table[SPA_FEATURE_MULTI_VDEV_CRASH_DUMP]) ?
	    ZIO_CHECKSUM_NOPARITY : ZIO_CHECKSUM_OFF;

	/*
	 * If we are resizing the dump device then we only need to
	 * update the refreservation to match the newly updated
	 * zvolsize. Otherwise, we save off the original state of the
	 * zvol so that we can restore them if the zvol is ever undumpified.
	 */
	if (resize) {
		error = zap_update(os, ZVOL_ZAP_OBJ,
		    zfs_prop_to_name(ZFS_PROP_REFRESERVATION), 8, 1,
		    &zv->zv_volsize, tx);
	} else {
		uint64_t checksum, compress, refresrv, vbs, dedup;

		error = dsl_prop_get_integer(zv->zv_name,
		    zfs_prop_to_name(ZFS_PROP_COMPRESSION), &compress, NULL);
		error = error ? error : dsl_prop_get_integer(zv->zv_name,
		    zfs_prop_to_name(ZFS_PROP_CHECKSUM), &checksum, NULL);
		error = error ? error : dsl_prop_get_integer(zv->zv_name,
		    zfs_prop_to_name(ZFS_PROP_REFRESERVATION), &refresrv, NULL);
		error = error ? error : dsl_prop_get_integer(zv->zv_name,
		    zfs_prop_to_name(ZFS_PROP_VOLBLOCKSIZE), &vbs, NULL);
		if (version >= SPA_VERSION_DEDUP) {
			error = error ? error :
			    dsl_prop_get_integer(zv->zv_name,
			    zfs_prop_to_name(ZFS_PROP_DEDUP), &dedup, NULL);
		}

		error = error ? error : zap_update(os, ZVOL_ZAP_OBJ,
		    zfs_prop_to_name(ZFS_PROP_COMPRESSION), 8, 1,
		    &compress, tx);
		error = error ? error : zap_update(os, ZVOL_ZAP_OBJ,
		    zfs_prop_to_name(ZFS_PROP_CHECKSUM), 8, 1, &checksum, tx);
		error = error ? error : zap_update(os, ZVOL_ZAP_OBJ,
		    zfs_prop_to_name(ZFS_PROP_REFRESERVATION), 8, 1,
		    &refresrv, tx);
		error = error ? error : zap_update(os, ZVOL_ZAP_OBJ,
		    zfs_prop_to_name(ZFS_PROP_VOLBLOCKSIZE), 8, 1,
		    &vbs, tx);
		error = error ? error : dmu_object_set_blocksize(
		    os, ZVOL_OBJ, SPA_MAXBLOCKSIZE, 0, tx);
		if (version >= SPA_VERSION_DEDUP) {
			error = error ? error : zap_update(os, ZVOL_ZAP_OBJ,
			    zfs_prop_to_name(ZFS_PROP_DEDUP), 8, 1,
			    &dedup, tx);
		}
		if (error == 0)
			zv->zv_volblocksize = SPA_MAXBLOCKSIZE;
	}
	dmu_tx_commit(tx);

	/*
	 * We only need update the zvol's property if we are initializing
	 * the dump area for the first time.
	 */
	if (!resize) {
		VERIFY(nvlist_alloc(&nv, NV_UNIQUE_NAME, KM_SLEEP) == 0);
		VERIFY(nvlist_add_uint64(nv,
		    zfs_prop_to_name(ZFS_PROP_REFRESERVATION), 0) == 0);
		VERIFY(nvlist_add_uint64(nv,
		    zfs_prop_to_name(ZFS_PROP_COMPRESSION),
		    ZIO_COMPRESS_OFF) == 0);
		VERIFY(nvlist_add_uint64(nv,
		    zfs_prop_to_name(ZFS_PROP_CHECKSUM),
		    checksum) == 0);
		if (version >= SPA_VERSION_DEDUP) {
			VERIFY(nvlist_add_uint64(nv,
			    zfs_prop_to_name(ZFS_PROP_DEDUP),
			    ZIO_CHECKSUM_OFF) == 0);
		}

		error = zfs_set_prop_nvlist(zv->zv_name, ZPROP_SRC_LOCAL,
		    nv, NULL);
		nvlist_free(nv);

		if (error)
			return (error);
	}

	/* Allocate the space for the dump */
	error = zvol_prealloc(zv);
	return (error);
}

static int
zvol_dumpify(zvol_state_t *zv)
{
	int error = 0;
	uint64_t dumpsize = 0;
	dmu_tx_t *tx;
	objset_t *os = zv->zv_objset;

	if (zv->zv_flags & ZVOL_RDONLY)
		return (SET_ERROR(EROFS));

	if (zap_lookup(zv->zv_objset, ZVOL_ZAP_OBJ, ZVOL_DUMPSIZE,
	    8, 1, &dumpsize) != 0 || dumpsize != zv->zv_volsize) {
		boolean_t resize = (dumpsize > 0);

		if ((error = zvol_dump_init(zv, resize)) != 0) {
			(void) zvol_dump_fini(zv);
			return (error);
		}
	}

	/*
	 * Build up our lba mapping.
	 */
	error = zvol_get_lbas(zv);
	if (error) {
		(void) zvol_dump_fini(zv);
		return (error);
	}

	tx = dmu_tx_create(os);
	dmu_tx_hold_zap(tx, ZVOL_ZAP_OBJ, TRUE, NULL);
	error = dmu_tx_assign(tx, TXG_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		(void) zvol_dump_fini(zv);
		return (error);
	}

	zv->zv_flags |= ZVOL_DUMPIFIED;
	error = zap_update(os, ZVOL_ZAP_OBJ, ZVOL_DUMPSIZE, 8, 1,
	    &zv->zv_volsize, tx);
	dmu_tx_commit(tx);

	if (error) {
		(void) zvol_dump_fini(zv);
		return (error);
	}

	txg_wait_synced(dmu_objset_pool(os), 0);
	return (0);
}

static int
zvol_dump_fini(zvol_state_t *zv)
{
	dmu_tx_t *tx;
	objset_t *os = zv->zv_objset;
	nvlist_t *nv;
	int error = 0;
	uint64_t checksum, compress, refresrv, vbs, dedup;
	uint64_t version = spa_version(dmu_objset_spa(zv->zv_objset));

	/*
	 * Attempt to restore the zvol back to its pre-dumpified state.
	 * This is a best-effort attempt as it's possible that not all
	 * of these properties were initialized during the dumpify process
	 * (i.e. error during zvol_dump_init).
	 */

	tx = dmu_tx_create(os);
	dmu_tx_hold_zap(tx, ZVOL_ZAP_OBJ, TRUE, NULL);
	error = dmu_tx_assign(tx, TXG_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		return (error);
	}
	(void) zap_remove(os, ZVOL_ZAP_OBJ, ZVOL_DUMPSIZE, tx);
	dmu_tx_commit(tx);

	(void) zap_lookup(zv->zv_objset, ZVOL_ZAP_OBJ,
	    zfs_prop_to_name(ZFS_PROP_CHECKSUM), 8, 1, &checksum);
	(void) zap_lookup(zv->zv_objset, ZVOL_ZAP_OBJ,
	    zfs_prop_to_name(ZFS_PROP_COMPRESSION), 8, 1, &compress);
	(void) zap_lookup(zv->zv_objset, ZVOL_ZAP_OBJ,
	    zfs_prop_to_name(ZFS_PROP_REFRESERVATION), 8, 1, &refresrv);
	(void) zap_lookup(zv->zv_objset, ZVOL_ZAP_OBJ,
	    zfs_prop_to_name(ZFS_PROP_VOLBLOCKSIZE), 8, 1, &vbs);

	VERIFY(nvlist_alloc(&nv, NV_UNIQUE_NAME, KM_SLEEP) == 0);
	(void) nvlist_add_uint64(nv,
	    zfs_prop_to_name(ZFS_PROP_CHECKSUM), checksum);
	(void) nvlist_add_uint64(nv,
	    zfs_prop_to_name(ZFS_PROP_COMPRESSION), compress);
	(void) nvlist_add_uint64(nv,
	    zfs_prop_to_name(ZFS_PROP_REFRESERVATION), refresrv);
	if (version >= SPA_VERSION_DEDUP &&
	    zap_lookup(zv->zv_objset, ZVOL_ZAP_OBJ,
	    zfs_prop_to_name(ZFS_PROP_DEDUP), 8, 1, &dedup) == 0) {
		(void) nvlist_add_uint64(nv,
		    zfs_prop_to_name(ZFS_PROP_DEDUP), dedup);
	}
	(void) zfs_set_prop_nvlist(zv->zv_name, ZPROP_SRC_LOCAL,
	    nv, NULL);
	nvlist_free(nv);

	zvol_free_extents(zv);
	zv->zv_flags &= ~ZVOL_DUMPIFIED;
	(void) dmu_free_long_range(os, ZVOL_OBJ, 0, DMU_OBJECT_END);
	/* wait for dmu_free_long_range to actually free the blocks */
	txg_wait_synced(dmu_objset_pool(zv->zv_objset), 0);
	tx = dmu_tx_create(os);
	dmu_tx_hold_bonus(tx, ZVOL_OBJ);
	error = dmu_tx_assign(tx, TXG_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		return (error);
	}
	if (dmu_object_set_blocksize(os, ZVOL_OBJ, vbs, 0, tx) == 0)
		zv->zv_volblocksize = vbs;
	dmu_tx_commit(tx);

	return (0);
}
