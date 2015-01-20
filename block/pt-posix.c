/*
 * ATAPI guest commands translation.
 *
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
#include "qemu/module.h"
#include "block/block_int.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <linux/cdrom.h>
#include <linux/fd.h>
#include <linux/fs.h>

/* BDRVPassThrough Definition */
#include "block/pt.h"

/* -- Pass Through function and definitions -------------------------------- */
/* TODO: Maybe should be in /var/lib/qemu/ directory */
#define PT_SHM_NAME_TEMPLATE "/xen-atapi-pt-status-%04x:%04x"

/* The template use to determine the lock_file_name field in
 * BDRVPosixPassThroughState */
#define PT_EXCLUSIVE_CD_FILE_TEMPLATE "/var/lock/xen-cd-exclusive-%04x:%04x"

#define DEBUG_PTPOSIX 0
#define PTPOSIX_TAG "pt-posix:"
#define DPRINTF(fmt, ...)                                           \
    do {                                                            \
        if (DEBUG_PTPOSIX)                                          \
            fprintf(stdout, PTPOSIX_TAG"%s:%d: " fmt "\n",          \
                    "block/pt-posix.c", __LINE__, ##__VA_ARGS__);   \
    } while (0)

struct ATAPIShm {
    enum ATAPIMediaState mediastate;
};

typedef struct BDRVPosixPassThroughState {
    /* The Device File Descriptor */
    int fd;
    /* The open flags */
    int open_flags;

    /* SHared Memory */
    int                  shmfd;
    char                 shm_name[1024];
    struct ATAPIShm     *volatile shm;
    enum ATAPIMediaState lastmediastate;

    /* Lock File Name */
    char                lock_file_name[1024];
} BDRVPosixPassThroughState;

static int pt_receive_request_from_device(BlockDriverState *bs,
                                          uint32_t const cmd)
{
    BDRVPosixPassThroughState *bdrvpts = bs->opaque;

    switch (cmd) {
    case BLOCK_PT_CMD_SET_MEDIA_STATE_UNKNOWN:
        bdrvpts->shm->mediastate = MEDIA_STATE_UNKNOWN;
        break;
    case BLOCK_PT_CMD_SET_MEDIA_PRESENT:
        bdrvpts->shm->mediastate = MEDIA_PRESENT;
	bdrvpts->lastmediastate = MEDIA_PRESENT;
        break;
    case BLOCK_PT_CMD_SET_MEDIA_ABSENT:
        /* TODO: No media, remove exclusivity lock */
        bdrvpts->shm->mediastate = MEDIA_ABSENT;
	bdrvpts->lastmediastate = MEDIA_ABSENT;
        break;
    case BLOCK_PT_CMD_ERROR:
    default:
        return -ENOTSUP;
    }

    return 0;
}

static int pt_send_data_to_device(BlockDriverState *bs,
                                  uint32_t const cmd, uint32_t *data)
{
    BDRVPosixPassThroughState *bdrvpts = bs->opaque;

    switch (cmd) {
    case BLOCK_PT_CMD_GET_LASTMEDIASTATE:
        *data = bdrvpts->lastmediastate;
        break;
    case BLOCK_PT_CMD_GET_SHM_MEDIASTATE:
        *data = bdrvpts->shm->mediastate;
        break;
    case BLOCK_PT_CMD_ERROR:
    default:
        return -ENOTSUP;
    }

    return 0;
}


static int pt_posix_init_shm(BDRVPosixPassThroughState *bdrvpts)
{
    struct stat stat;
    int ret = -1;

    if (fstat(bdrvpts->fd, &stat)) {
        fprintf(stderr, "fstat() failed (%s).\n", strerror(errno));
        goto pt_posix_init_shm_error;
    }
    memset(bdrvpts->shm_name, 0, sizeof (bdrvpts->shm_name));
    snprintf(bdrvpts->shm_name, sizeof (bdrvpts->shm_name) - 1,
             PT_SHM_NAME_TEMPLATE,
             major(stat.st_rdev), minor(stat.st_rdev));
    /* @open the SHared Memory */
    bdrvpts->shmfd = shm_open(bdrvpts->shm_name, O_CREAT | O_RDWR, 0666);
    if (bdrvpts->shmfd < 0) {
        fprintf(stderr, "shm_open() failed (%s).\n", strerror(errno));
        goto pt_posix_init_shm_error;
    }
    if (ftruncate(bdrvpts->shmfd, sizeof(*(bdrvpts->shm))) == -1) {
        fprintf(stderr, "ftruncate() failed (%s).\n", strerror(errno));
        goto pt_posix_init_shm_error_unlink;
    }
    /* @map the SHared Memory */
    /* TODO: Unmap it at the close of the driver!!! if needed ?*/
    bdrvpts->shm = mmap(NULL, sizeof (*(bdrvpts->shm)), PROT_READ|PROT_WRITE,
                        MAP_SHARED, bdrvpts->shmfd, 0);
    if (bdrvpts->shm == MAP_FAILED) {
        fprintf(stderr, "mmap() failed (%s).\n", strerror(errno));
        goto pt_posix_init_shm_error_unmap;
    }

    /* @prepare the lock file name for a future check */
    memset(bdrvpts->lock_file_name, 0, sizeof (bdrvpts->lock_file_name));
    snprintf(bdrvpts->lock_file_name, sizeof (bdrvpts->lock_file_name) - 1,
             PT_EXCLUSIVE_CD_FILE_TEMPLATE,
             major(stat.st_rdev), minor(stat.st_rdev));

    return 0;

pt_posix_init_shm_error_unmap:
    munmap(bdrvpts->shm, sizeof (*(bdrvpts->shm)));
pt_posix_init_shm_error_unlink:
    shm_unlink(bdrvpts->shm_name);
pt_posix_init_shm_error:
    return ret;
}
/* ------------------------------------------------------------------------- */

static void common_parse_flags(int bdrv_flags, int *open_flags)
{
    assert(open_flags != NULL);

    *open_flags |= O_BINARY;
    *open_flags &= ~O_ACCMODE;
    if (bdrv_flags & BDRV_O_RDWR) {
        *open_flags |= O_RDWR;
    } else {
        *open_flags |= O_RDONLY;
    }

    /* Use O_DSYNC for write-through caching, no flags for write-back caching,
     * and O_DIRECT for no caching. */
    if ((bdrv_flags & BDRV_O_NOCACHE)) {
        *open_flags |= O_DIRECT;
    }
}

static int common_open(BlockDriverState *bs, const char *filename,
                       int bdrv_flags, int open_flags)
{
    BDRVPosixPassThroughState *s = bs->opaque;
    int fd, ret;

    s->open_flags = open_flags;
    common_parse_flags(bdrv_flags, &s->open_flags);

    s->fd = -1;
    fd = qemu_open(filename, s->open_flags, 0644);

    if (fd < 0) {
        ret = -errno;
        if (ret == -EROFS)
            ret = -EACCES;
        return ret;
    }
    s->fd = fd;

    return 0;
}

static void common_close(BlockDriverState* bs)
{
    BDRVPosixPassThroughState *s = bs->opaque;

    if (s->fd >= 0) {
        qemu_close(s->fd);
        s->fd = -1;
    }
}

static int pt_probe_device(const char *filename)
{
    int fd, ret;
    int prio = 0;
    struct stat st;

    fd = qemu_open(filename, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        goto out;
    }
    ret = fstat(fd, &st);
    if (ret == -1 || !S_ISBLK(st.st_mode)) {
        goto outc;
    }

    /* Attempt to detect via a CDROM specific ioctl */
    ret = ioctl(fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
    if (ret >= 0)
        prio = 100;

outc:
    qemu_close(fd);
out:
    return prio;
}

static int pt_open(BlockDriverState *bs, const char *filename, int flags)
{
    BDRVPosixPassThroughState *bdrvpts = bs->opaque;
    int ret;

    ret = common_open(bs, filename, flags, O_NONBLOCK | O_RDWR);

    if (!ret) {
        ret = pt_posix_init_shm(bdrvpts);
    }

    return ret;
}

static int pt_ioctl(BlockDriverState *bs, unsigned long int req, void *buf)
{
    BDRVPosixPassThroughState *s = bs->opaque;
    int ret;

    ret = ioctl(s->fd, req, buf);
    if (ret < 0) {
        fprintf(stderr, "ioctl() failed (%s).\n", strerror(errno));
    }
    return ret;
}

static BlockDriver bdrv_host_posix_pt = {
    .format_name         = "pt-posix",
    .protocol_name       = "pt-posix",
    .instance_size       = sizeof(BDRVPosixPassThroughState),
    .bdrv_probe_device   = pt_probe_device,
    .bdrv_file_open      = pt_open,
    .bdrv_close          = common_close,

    .bdrv_receive_request_from_device = pt_receive_request_from_device,
    .bdrv_send_data_to_device = pt_send_data_to_device,

    .bdrv_ioctl          = pt_ioctl,
};

#include <linux/cdrom.h>
#include <linux/bsg.h>
#include <scsi/sg.h>
#include <libv4v.h>

#define V4V_TYPE 'W'
#define V4VIOCSETRINGSIZE       _IOW (V4V_TYPE,  1, uint32_t)

#define ATAPI_CDROM_PORT 5000
#define V4V_ATAPI_PT_RING_SIZE \
  (V4V_ROUNDUP((((4096)*64) - sizeof(v4v_ring_t)-V4V_ROUNDUP(1))))

typedef enum v4vcmd {
    ATAPI_PT_OPEN      = 0x00,
    ATAPI_PT_CLOSE     = 0x01,
    ATAPI_PT_IOCTL     = 0x02,
    ATAPI_PT_SET_STATE = 0x03,
    ATAPI_PT_GET_STATE = 0x04,

    ATAPI_PT_NUMBER_OF_COMMAND
} ptv4vcmd;

#define MAX_V4V_MSG_SIZE (V4V_ATAPI_PT_RING_SIZE)

typedef struct BDRVStubdomPassThroughState {
    int v4v_fd;

    v4v_addr_t remote_addr;
    v4v_addr_t local_addr;
    uint8_t io_buf[MAX_V4V_MSG_SIZE];
    uint32_t max_xfer_len;
    int stubdom_id;
    uint8_t dev_id;

    enum ATAPIMediaState lastmediastate;
} BDRVStubdomPassThroughState;

static int pt_v4v_open_common(BDRVStubdomPassThroughState* pts,
                              char const* filename)
{
    uint8_t io_buf[MAX_V4V_MSG_SIZE];
    int dev_name_len = strlen(filename);
    uint32_t v4v_ring_size = V4V_ATAPI_PT_RING_SIZE;
    int ret = -1;

    DPRINTF("%s: Open v4v socket.", __FUNCTION__);
    pts->v4v_fd = v4v_socket(SOCK_DGRAM);
    if (pts->v4v_fd < 0) {
        fprintf(stderr, "v4v_socket() failed (%s).\n", strerror(errno));
        goto exit;
    }

    pts->local_addr.port = V4V_PORT_NONE;
    pts->local_addr.domain = V4V_DOMID_ANY;

    pts->remote_addr.port = ATAPI_CDROM_PORT;
    pts->remote_addr.domain = 0;

    DPRINTF("%s: Set v4v ring size.", __FUNCTION__);
    ioctl(pts->v4v_fd, V4VIOCSETRINGSIZE, &v4v_ring_size);

    DPRINTF("%s: Bind v4v socket with remote.", __FUNCTION__);
    if (v4v_bind(pts->v4v_fd, &pts->local_addr, 0)) {
        v4v_close(pts->v4v_fd);
        pts->v4v_fd = -1;
        fprintf(stderr, "v4v_bind() failed (%s).\n", strerror(errno));
        goto exit;
    }

    io_buf[0] = ATAPI_PT_OPEN;

    memcpy(&io_buf[1], filename, dev_name_len);
    io_buf[dev_name_len + 1] = '\0';

    DPRINTF("%s: send ATAPI_PT_OPEN through v4v.", __FUNCTION__);
    ret = v4v_sendto(pts->v4v_fd, io_buf, dev_name_len + 2, 0, &pts->remote_addr);
    if (ret != dev_name_len + 2) {
        fprintf(stderr, "v4v_sendto() failed (%s).\n", strerror(errno));
        ret = -1;
        v4v_close(pts->v4v_fd);
        goto exit;
    }

    ret = v4v_recvfrom(pts->v4v_fd, io_buf, 4, 0, &pts->remote_addr);
    DPRINTF("%s: recv %c%c.", __FUNCTION__, io_buf[1], io_buf[2]);
    if (io_buf[1] != 'o' || io_buf[2] != 'k') {
        fprintf(stderr, "v4v_recvfrom() failed (%s).\n", strerror(errno));
        v4v_close(pts->v4v_fd);
        pts->v4v_fd = -1;
        goto exit;
    }

    pts->dev_id = io_buf[3];
    ret = 0;
exit:
    return ret;
}

static int pt_v4v_close_common(BDRVStubdomPassThroughState* pts)
{
    uint8_t io_buf[MAX_V4V_MSG_SIZE];
    int ret = -1;

    if (pts->v4v_fd == -1) {
        DPRINTF("%s: v4v connection not initialized.", __FUNCTION__);
        goto exit;
    }

    io_buf[0] = ATAPI_PT_CLOSE;
    io_buf[1] = pts->dev_id;
    DPRINTF("%s: send ATAPI_PT_CLOSE through v4v.", __FUNCTION__);
    v4v_sendto(pts->v4v_fd, io_buf, 2, 0, &pts->remote_addr);

    ret = 0;
exit:
    return ret;
}

static void pt_v4v_close(BlockDriverState* bs)
{
    BDRVStubdomPassThroughState* pts = bs->opaque;

    if (pt_v4v_close_common(pts) == -1) {
        DPRINTF("%s: Could not close v4v connection for \"%s\".",
                __FUNCTION__, bs->filename);
    }
}

static int pt_v4v_open(BlockDriverState *bs, const char *filename, int flags)
{
    BDRVStubdomPassThroughState* pts = bs->opaque;
    (void) flags;

    return pt_v4v_open_common(pts, filename);
}

static int pt_v4v_probe_device(const char *filename)
{
    BDRVStubdomPassThroughState pts;
    int ret;
    int prio = 0;

    memset(&pts, 0, sizeof (BDRVStubdomPassThroughState));

    ret = pt_v4v_open_common(&pts, filename);
    if (ret == -1) {
        DPRINTF("%s: Cannot open \"%s\".", __FUNCTION__, filename);
        goto out;
    }

    ret = pt_v4v_close_common(&pts);
    if (ret == -1) {
        DPRINTF("%s: Cannot close \"%s\".", __FUNCTION__, filename);
        goto out;
    }

    prio = 100;
    DPRINTF("probe device \"%s\" with prio %d", filename, prio);
out:
    return prio;
}

static int pt_v4v_receive_request_from_device(BlockDriverState *bs,
                                          uint32_t const cmd)
{
    BDRVStubdomPassThroughState* pts = bs->opaque;

    uint8_t io_buf[MAX_V4V_MSG_SIZE];
    int ret = -1;

    if (pts->v4v_fd == -1) {
        DPRINTF("%s: v4v connection not initialized.", __FUNCTION__);
        goto exit;
    }

    io_buf[0] = ATAPI_PT_SET_STATE;
    io_buf[1] = pts->dev_id;
    io_buf[2] = (cmd & 0xFF);

    DPRINTF("%s: send ATAPI_PT_SET_STATE through v4v.", __FUNCTION__);
    ret = v4v_sendto(pts->v4v_fd, io_buf, 3, 0, &pts->remote_addr);
    if (ret != 3) {
        fprintf(stderr, "v4v_sendto() failed (%s).\n", strerror(errno));
        ret = -1;
        v4v_close(pts->v4v_fd);
        goto exit;
    }

    v4v_recvfrom(pts->v4v_fd, io_buf, 3, 0, &pts->remote_addr);
    DPRINTF("%s: recv %c%c.", __FUNCTION__, io_buf[1], io_buf[2]);
    if (io_buf[1] != 'o' || io_buf[2] != 'k') {
        v4v_close(pts->v4v_fd);
        pts->v4v_fd = -1;
        goto exit;
    }
    ret = 0;
exit:
    return ret;
}

static int pt_v4v_send_data_to_device(BlockDriverState *bs,
                                  uint32_t const cmd, uint32_t *data)
{
    BDRVStubdomPassThroughState* pts = bs->opaque;
    uint8_t io_buf[MAX_V4V_MSG_SIZE];
    int ret = -1;

    if (pts->v4v_fd == -1) {
        DPRINTF("%s: v4v connection not initialized.", __FUNCTION__);
        goto exit;
    }

    io_buf[0] = ATAPI_PT_GET_STATE;
    io_buf[1] = pts->dev_id;
    io_buf[2] = (cmd & 0xFF);
    io_buf[3] = 0x00;

    DPRINTF("%s: send ATAPI_PT_GET_STATE through v4v.", __FUNCTION__);
    ret = v4v_sendto(pts->v4v_fd, io_buf, 4, 0, &pts->remote_addr);
    if (ret != 4) {
        fprintf(stderr, "v4v_sendto() failed (%s).\n", strerror(errno));
        ret = -1;
        v4v_close(pts->v4v_fd);
        goto exit;
    }

    v4v_recvfrom(pts->v4v_fd, io_buf, 4, 0, &pts->remote_addr);
    DPRINTF("%s: recv %c%c.", __FUNCTION__, io_buf[1], io_buf[2]);
    if (io_buf[1] != 'o' || io_buf[2] != 'k') {
        fprintf(stderr, "v4v_recvfrom() failed (%s).\n", strerror(errno));
        v4v_close(pts->v4v_fd);
        pts->v4v_fd = -1;
        goto exit;
    }
    *data = io_buf[3];
    ret = 0;
exit:
    return ret;
}

#if 0
static void dump_hex(uint8_t* p, size_t len)
{
  int i, j;
  char buf[80];
  int index;

  for (i = 0; i < len; i += 16) {
    memset(buf, 0, sizeof(buf));
    index = 0;
    for (j = 0; (j < 16) && ((i + j) < len); j++) {
      index += snprintf(&buf[index], sizeof(buf) - index, "%02x ", p[i+j]);
    }
    for (; j < 16; j++) {
      index += snprintf(&buf[index], sizeof(buf) - index, "   ");
    }
    index += snprintf(&buf[index], sizeof(buf) - index, " ");
    for (j = 0; (j < 16) && ((i + j) < len); j++) {
      index += snprintf(&buf[index], sizeof(buf) - index,
                        "%c",
                        ((p[i+j] < ' ') || (p[i+j] > 0x7e)) ? '.' : p[i+j]);
    }
    fprintf(stderr, "%s\n", buf);
  }
}
#endif

static int pt_v4v_ioctl(BlockDriverState *bs, unsigned long int req, void *buf)
{
    BDRVStubdomPassThroughState* pts = bs->opaque;
    uint8_t io_buf[MAX_V4V_MSG_SIZE];
    int ret = -1;
    int len;

    struct sg_io_v4* cmd;
    struct request_sense* sense;
    uint8_t* sg_buf;
    size_t current_transfer_len = 0;
    int is_dout = 0;
    uint8_t *copy_of_cmd_din_xferp = NULL;

    if (pts->v4v_fd == -1) {
        DPRINTF("%s: v4v connection not initialized.", __FUNCTION__);
        goto exit;
    }

    io_buf[0] = ATAPI_PT_IOCTL;
    io_buf[1] = pts->dev_id;

    io_buf[2] = ((req & 0xFF000000) >> 24) & 0x00FF;
    io_buf[3] = ((req & 0x00FF0000) >> 16) & 0x00FF;
    io_buf[4] = ((req & 0x0000FF00) >>  8) & 0x00FF;
    io_buf[5] = ((req & 0x000000FF) >>  0) & 0x00FF;
    len = 6;

    switch (req) {
    case SG_GET_RESERVED_SIZE:
        DPRINTF("%s: send ATAPI_PT_IOCTL(%#lx) through v4v.", __FUNCTION__, req);
        v4v_sendto(pts->v4v_fd, io_buf, len, 0, &pts->remote_addr);

        len = v4v_recvfrom(pts->v4v_fd, io_buf, len + 1, 0, &pts->remote_addr);
        DPRINTF("%s: recv %c%c.", __FUNCTION__, io_buf[1], io_buf[2]);
        if (io_buf[1] != 'o' || io_buf[2] != 'k') {
            v4v_close(pts->v4v_fd);
            pts->v4v_fd = -1;
            goto exit;
        }

	memcpy(buf, &io_buf[3], sizeof(uint32_t));
        break;
    case SG_IO:
        sg_buf = &io_buf[len];

        cmd = (struct sg_io_v4*) buf;
	sense = (struct request_sense*)(uintptr_t)cmd->response;

        memcpy (sg_buf, cmd, sizeof(struct sg_io_v4));
        sg_buf += sizeof (struct sg_io_v4);
        len += sizeof (struct sg_io_v4);

	memcpy (sg_buf, (uint8_t*)(uintptr_t)cmd->request, cmd->request_len);
	sg_buf += cmd->request_len;
        len += cmd->request_len;

        if(cmd->dout_xfer_len > 0) {
            is_dout = 1;
            current_transfer_len = cmd->dout_xfer_len;
            memcpy(sg_buf, (uint8_t*)(uintptr_t)cmd->dout_xferp, cmd->dout_xfer_len);
            sg_buf += cmd->dout_xfer_len;
            len += cmd->dout_xfer_len;
        } else {
            current_transfer_len = cmd->din_xfer_len;
            copy_of_cmd_din_xferp = (uint8_t*)(uintptr_t)cmd->din_xferp;
	}

        DPRINTF("%s: send ATAPI_CMD(%#02x) through v4v.", __FUNCTION__,
                *(uint8_t*)(uintptr_t)cmd->request);
        v4v_sendto(pts->v4v_fd, io_buf, len, 0, &pts->remote_addr);

        len = v4v_recvfrom(pts->v4v_fd, io_buf, MAX_V4V_MSG_SIZE, 0, &pts->remote_addr);
        DPRINTF("%s: recv %c%c.", __FUNCTION__, io_buf[1], io_buf[2]);
        if (io_buf[1] != 'o' || io_buf[2] != 'k') {
            v4v_close(pts->v4v_fd);
            pts->v4v_fd = -1;
            ret = -1;
            goto exit;
        }

	sg_buf = &io_buf[6];
	memcpy(cmd, sg_buf, sizeof (struct sg_io_v4));
        sg_buf += sizeof (struct sg_io_v4);

	memcpy((uint8_t *)sense, sg_buf, cmd->max_response_len);
	sg_buf += cmd->max_response_len;
        if (!is_dout) {
            memcpy(copy_of_cmd_din_xferp, sg_buf, current_transfer_len);
            sg_buf += current_transfer_len;
        }
        break;
    default:
        DPRINTF("%s: ioctl(%#08lx) not handled for pass-through.", __FUNCTION__, req);
        goto exit;
    }

    ret = 0;
exit:
    return ret;
}

static BlockDriver bdrv_host_stubdom_pt = {
    .format_name         = "pt-stubdom",
    .protocol_name       = "pt-stubdom",
    .instance_size       = sizeof(BDRVStubdomPassThroughState),
    .bdrv_probe_device   = pt_v4v_probe_device,
    .bdrv_file_open      = pt_v4v_open,
    .bdrv_close          = pt_v4v_close,

    .bdrv_receive_request_from_device = pt_v4v_receive_request_from_device,
    .bdrv_send_data_to_device = pt_v4v_send_data_to_device,

    .bdrv_ioctl          = pt_v4v_ioctl,
};


static void bdrv_pt_init(void)
{
    /*
     * Register all the drivers.  Note that order is important, the driver
     * registered last will get probed first.
     */
    bdrv_register(&bdrv_host_stubdom_pt);
    bdrv_register(&bdrv_host_posix_pt);
}

block_init(bdrv_pt_init);
