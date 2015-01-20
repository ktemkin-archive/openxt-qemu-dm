/*
 * Copyright (C) 2014 Citrix Systems Ltd
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "qemu-common.h"
#include "block/block_int.h"
#include "qemu/module.h"

static int pt_open(BlockDriverState *bs, int flags)
{
    bs->sg = bs->file->sg;
    return 0;
}

/* We have nothing to do for pt reopen, stubs just return
 * success */
static int pt_reopen_prepare(BDRVReopenState *state,
                              BlockReopenQueue *queue,  Error **errp)
{
    return 0;
}

static int coroutine_fn pt_co_readv(BlockDriverState *bs, int64_t sector_num,
                                     int nb_sectors, QEMUIOVector *qiov)
{
    BLKDBG_EVENT(bs->file, BLKDBG_READ_AIO);
    return bdrv_co_readv(bs->file, sector_num, nb_sectors, qiov);
}

static int coroutine_fn pt_co_writev(BlockDriverState *bs, int64_t sector_num,
                                      int nb_sectors, QEMUIOVector *qiov)
{
    BLKDBG_EVENT(bs->file, BLKDBG_WRITE_AIO);
    return bdrv_co_writev(bs->file, sector_num, nb_sectors, qiov);
}

static void pt_close(BlockDriverState *bs)
{
}

static int coroutine_fn pt_co_is_allocated(BlockDriverState *bs,
                                            int64_t sector_num,
                                            int nb_sectors, int *pnum)
{
    return bdrv_co_is_allocated(bs->file, sector_num, nb_sectors, pnum);
}

static int64_t pt_getlength(BlockDriverState *bs)
{
    return bdrv_getlength(bs->file);
}

static int pt_truncate(BlockDriverState *bs, int64_t offset)
{
    return bdrv_truncate(bs->file, offset);
}

static int pt_probe(const uint8_t *buf, int buf_size, const char *filename)
{
   return 1; /* everything can be opened as pt image */
}

static int coroutine_fn pt_co_discard(BlockDriverState *bs,
                                       int64_t sector_num, int nb_sectors)
{
    return bdrv_co_discard(bs->file, sector_num, nb_sectors);
}

static int pt_is_inserted(BlockDriverState *bs)
{
    return bdrv_is_inserted(bs->file);
}

static int pt_media_changed(BlockDriverState *bs)
{
    return bdrv_media_changed(bs->file);
}

static void pt_eject(BlockDriverState *bs, bool eject_flag)
{
    bdrv_eject(bs->file, eject_flag);
}

static void pt_lock_medium(BlockDriverState *bs, bool locked)
{
    bdrv_lock_medium(bs->file, locked);
}

static int pt_ioctl(BlockDriverState *bs, unsigned long int req, void *buf)
{
   return bdrv_ioctl(bs->file, req, buf);
}

static BlockDriverAIOCB *pt_aio_ioctl(BlockDriverState *bs,
        unsigned long int req, void *buf,
        BlockDriverCompletionFunc *cb, void *opaque)
{
   return bdrv_aio_ioctl(bs->file, req, buf, cb, opaque);
}

static int pt_create(const char *filename, QEMUOptionParameter *options)
{
    return bdrv_create_file(filename, options);
}

static QEMUOptionParameter pt_create_options[] = {
    { NULL }
};

static int pt_has_zero_init(BlockDriverState *bs)
{
    return bdrv_has_zero_init(bs->file);
}

static BlockDriver bdrv_pt = {
    .format_name        = "pt",

    /* It's really 0, but we need to make g_malloc() happy */
    .instance_size      = 1,

    .bdrv_open          = pt_open,
    .bdrv_close         = pt_close,

    .bdrv_reopen_prepare  = pt_reopen_prepare,

    .bdrv_co_readv          = pt_co_readv,
    .bdrv_co_writev         = pt_co_writev,
    .bdrv_co_is_allocated   = pt_co_is_allocated,
    .bdrv_co_discard        = pt_co_discard,

    .bdrv_probe         = pt_probe,
    .bdrv_getlength     = pt_getlength,
    .bdrv_truncate      = pt_truncate,

    .bdrv_is_inserted   = pt_is_inserted,
    .bdrv_media_changed = pt_media_changed,
    .bdrv_eject         = pt_eject,
    .bdrv_lock_medium   = pt_lock_medium,

    .bdrv_ioctl         = pt_ioctl,
    .bdrv_aio_ioctl     = pt_aio_ioctl,

    .bdrv_create        = pt_create,
    .create_options     = pt_create_options,
    .bdrv_has_zero_init = pt_has_zero_init,
};

static void bdrv_pt_init(void)
{
    bdrv_register(&bdrv_pt);
}

block_init(bdrv_pt_init);
