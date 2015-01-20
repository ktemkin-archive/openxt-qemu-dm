#include "surfman.h"

/* HACK: Offset at which we copy ds->surface if the buffer is not shared between the emulation and the guest. */
#define HIDDEN_LFB_OFFSET   0xa00000

static struct SurfmanState *ss = NULL;
/*
 * DisplayState is created by the "hardware" through graphic_console_init().
 */

/* Called every DisplayChangeListener::gui_timer_interval. */
static void surfman_dpy_refresh(struct DisplayState *ds)
{
    (void) ds;
    vga_hw_update();    /* "Hardware" updates the framebuffer. */
}

/* A rectangular portion of the framebuffer (Surface) of DisplayState /s/ has changed. */
static void surfman_dpy_gfx_update(struct DisplayState *ds, int x, int y, int w, int h)
{
    if (!is_buffer_shared(ds->surface)) {
        unsigned int linesize = ds_get_linesize(ds);     // Somehow, the pixman_image_t is 64b aligned ... always?
        unsigned int Bpp = ds_get_bytes_per_pixel(ds);
        uint8_t *dest = ss->vram_ptr + HIDDEN_LFB_OFFSET + y * linesize + x * Bpp;
        uint8_t *src = ds_get_data(ds) + y * linesize + x * Bpp;
        unsigned int i;

        surfman_debug("update vram:%#"HWADDR_PRIx" src:%p dest:%p %d,%d (%dx%d).",
                      ss->vram->addr, src, dest, x, y, w, h);
        for (i = 0; i < h; ++i) {
            memcpy(dest, src, w * Bpp);
            dest += linesize;
            src += linesize;
        }
    }
}

static void surfman_lfb_state_save(struct SurfmanState *ss)
{
    struct DisplayState *ds = ss->ds;

    ss->current.width = ds_get_width(ds);
    ss->current.height = ds_get_height(ds);
    ss->current.linesize = ds_get_linesize(ds);
    ss->current.format = surfman_get_format(ds_get_format(ds));
    ss->current.addr = ss->vram->addr;
}

static int surfman_lfb_state_compare(const struct lfb_state *s, struct DisplayState *ds)
{
    hwaddr lfb_addr;

    lfb_addr = is_buffer_shared(ds->surface) ? ss->vram->addr : ss->vram->addr + HIDDEN_LFB_OFFSET;
    return !((s->width == ds_get_width(ds)) &&
             (s->height == ds_get_height(ds)) &&
             (s->linesize == ds_get_linesize(ds)) &&
             (s->format == surfman_get_format(ds_get_format(ds))) &&
             (s->addr == lfb_addr));
}

/* The geometry of the framebuffer (Surface) of DisplayState has changed. */
static void surfman_dpy_gfx_resize(struct DisplayState *ds)
{
    struct msg_display_resize msg;
    struct msg_empty_reply reply;

    if (!surfman_lfb_state_compare(&ss->current, ds)) {
        return;
    }
    msg.DisplayID = 0;  /* Not supported anyway. */
    msg.width = ds_get_width(ds);
    msg.height = ds_get_height(ds);
    msg.linesize = ds_get_linesize(ds);
    msg.format = surfman_get_format(ds_get_format(ds));
    if (!msg.format) {
        surfman_error("Unsupported pixel format `%#x'.", ds_get_format(ds));
        return;
    }

    msg.fb_offset = 0;  // Legacy value ?
    if (is_buffer_shared(ds->surface)) {
        // VRAM is accessible through BAR0 and the linear framebuffer is accessible in it.
        msg.lfb_addr = ss->vram->addr;
        msg.lfb_traceable = 1;
    } else {
        msg.lfb_addr = ss->vram->addr + HIDDEN_LFB_OFFSET;
        msg.lfb_traceable = 0;
    }
    surfman_info("resize %dx%d:%d -> %dx%d:%d%s %s %s.",
                 ss->current.width, ss->current.height, ss->current.linesize,
                 msg.width, msg.height, msg.linesize,
                 msg.lfb_addr == ss->vram->addr ? " (shared)": "",
                 ds->have_text ? "have text" : "",
                 ds->have_gfx ? "have gfx" : "");

    dmbus_send(ss->dmbus_service, DMBUS_MSG_DISPLAY_RESIZE, &msg, sizeof (msg));
    dmbus_sync_recv(ss->dmbus_service, DMBUS_MSG_EMPTY_REPLY, &reply, sizeof (reply));
    surfman_lfb_state_save(ss);
}

/* The framebuffer (Surface) address has changed.
 * /!\ We don't have a specific RPC with Surfman for that, so recycle resize. */
static void surfman_dpy_gfx_setdata(struct DisplayState *ds)
{
    surfman_dpy_gfx_resize(ds);
}

//static void surfman_dpy_gfx_copy(struct DisplayState *s, int src_x, int src_y,
//                                 int dst_x, int dst_y, int w, int h);

//static void surfman_dpy_text_cursor(struct DisplayState *s, int x, int y);
//static void surfman_dpy_text_resize(struct DisplayState *s, int w, int h);
//static void surfman_dpy_text_update(struct DisplayState *s, int x, int y, int w, int h);

//static void surfman_dpy_mouse_set(struct DisplayState *s, int x, int y, int on);
//static void surfman_dpy_cursor_define(struct DisplayState *s, QEMUCursor *cursor);

static void surfman_dpy_get_display_limits(DisplayState *ds,
                                           unsigned int *width_max, unsigned int *height_max,
                                           unsigned int *stride_alignment)
{
    struct msg_display_get_info msg;
    struct msg_display_info reply;

    msg.DisplayID = 0;
    dmbus_send(ss->dmbus_service, DMBUS_MSG_DISPLAY_GET_INFO, &msg, sizeof (msg));
    dmbus_sync_recv(ss->dmbus_service, DMBUS_MSG_DISPLAY_INFO, &reply, sizeof (reply));

    if (width_max)
        *width_max = reply.max_xres;
    if (height_max)
        *height_max = reply.max_yres;
    if (stride_alignment)
        *stride_alignment = reply.align;

    surfman_debug("display_limits: %ux%u stride aligned on %u.", *width_max, *height_max, *stride_alignment);
}

static void surfman_on_reconnect(void *opaque)
{
    surfman_dpy_gfx_resize(ss->ds);
}

static struct dmbus_ops surfman_dmbus_ops = {
    .dom0_input_event = NULL,
    .dom0_input_pvm = NULL,
    .input_config = NULL,
    .input_config_reset = NULL,
    .display_info = NULL,
    .display_edid = NULL,
    .reconnect = surfman_on_reconnect
};

/* Initialize Surfman's change listener. */
void surfman_display_init(DisplayState *ds)
{
    DisplayChangeListener *dcl;

    surfman_info("Initialize Surfman display.");

    ss = g_malloc0(sizeof (*ss));
    ss->ds = ds;
    ss->vram = xen_get_framebuffer();
    if (!ss->vram) {
        surfman_error("Could not recover VRAM MemoryRegion.");
        goto err_vram;
    }
    ss->vram_ptr = memory_region_get_ram_ptr(ss->vram);
    ss->dmbus_service = dmbus_service_connect(DMBUS_SERVICE_SURFMAN, DEVICE_TYPE_VESA, &surfman_dmbus_ops, ss);
    if (!ss->dmbus_service) {
        surfman_error("Could not initialize dmbus.");
        goto err_dmbus;
    }

    dcl = g_malloc0(sizeof (*dcl));
    dcl->idle = 0;
    dcl->dpy_refresh = surfman_dpy_refresh;
    dcl->dpy_gfx_update = surfman_dpy_gfx_update;
    dcl->dpy_gfx_resize = surfman_dpy_gfx_resize;
    dcl->dpy_gfx_setdata = surfman_dpy_gfx_setdata;
    dcl->dpy_get_display_limits = surfman_dpy_get_display_limits;

    register_displaychangelistener(ds, dcl);
    return;

err_dmbus:
err_vram:
    g_free(ss);
}

