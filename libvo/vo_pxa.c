/*
 * Video driver for PXA27x/3xx Overlay 2
 * by Vasily Khoruzhick <anarsoul@gmail.com>
 * (C) 2007
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <linux/fb.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "config.h"
#include "video_out.h"
#include "video_out_internal.h"
#include "sub/sub.h"
#include "aspect.h"
#include "mp_msg.h"
#include "subopt-helper.h"

static vo_info_t info = {
    "PXA27x/3xx overlay",
    "pxa",
    "Vasily Khoruzhick <anarsoul@gmail.com>",
    ""
};

LIBVO_EXTERN(pxa);

/* VO driver state */

static int overlay_width, overlay_height, overlay_x, overlay_y;
static int overlay_fd = -1;
static void *overlay_mem;
static size_t overlay_mem_size;
static uint8_t *y_plane;
static uint8_t *u_plane;
static uint8_t *v_plane;

static int vo_pxa_check_format(uint32_t format)
{
    switch (format)
    {
        /* Planar YUV Formats */
        case IMGFMT_YV12:
        case IMGFMT_IYUV:
        case IMGFMT_I420:
            return( VFCAP_CSP_SUPPORTED | VFCAP_CSP_SUPPORTED_BY_HW
                    | VFCAP_ACCEPT_STRIDE );
            break;

    }

    mp_msg(MSGT_VO, MSGL_ERR, "vo_pxa: format %s is not supported\n",
        vo_format_name(format));

    return 0;
}

/*****************************************************************************
 * preinit
 *
 * Preinitializes driver
 *   arg - currently it's vo_subdevice
 *   returns: zero on successful initialization, non-zero on error.
 *
 ****************************************************************************/
static int preinit(const char *vo_subdevice)
{
    int r, fb = -1;
    struct fb_var_screeninfo fbvar;

    fb = open("/dev/fb0", O_RDWR);
    r = ioctl(fb, FBIOGET_VSCREENINFO, &fbvar);
    if (r) {
        mp_msg(MSGT_VO, MSGL_V, "vo_pxa: get_vscreeninfo ioctl failed");
        return -1;
    }

    overlay_width = fbvar.xres;
    overlay_height = fbvar.yres;

    mp_msg(MSGT_VO, MSGL_V, "vo_pxa: main plane res is %dx%d\n",
        overlay_width, overlay_height);

    close(fb);

    return 0;
}


/*****************************************************************************
 * config
 *
 * Config the display driver.
 * params:
 *   src_width,srcheight: image source size
 *   dst_width,dst_height: size of the requested window size, just a hint
 *   fullscreen: flag, 0=windowd 1=fullscreen, just a hint
 *   title: window title, if available
 *   format: fourcc of pixel format
 * returns : zero on successful initialization, non-zero on error.
 *
 ****************************************************************************/
static int config(uint32_t src_width, uint32_t src_height,
                  uint32_t dst_width, uint32_t dst_height, uint32_t flags,
                  char *title, uint32_t format)
{
    int r;
    struct fb_var_screeninfo fbvar;
    struct fb_fix_screeninfo fbfix;
    int i;

    mp_msg(MSGT_VO, MSGL_V, "vo_pxa: config() src_width:%d, src_height:%d, dst_width:%d, dst_height:%d\n",
       src_width, src_height, dst_width, dst_height);

    /* Check format */
    if (!vo_pxa_check_format(format))
        goto error;

    /* Check src image */
    if (src_width > overlay_width || src_height > overlay_height) {
        mp_msg(MSGT_VO, MSGL_ERR, "vo_pxa: src image is too big, scaling not supported.\n");
        goto error;
    }

    if (dst_width > overlay_width || dst_height > overlay_height) {
        mp_msg(MSGT_VO, MSGL_ERR, "vo_pxa: dst is too big, scaling not supported.\n");
        goto error;
    }

    if (src_width != dst_width || src_height != dst_height) {
        mp_msg(MSGT_VO, MSGL_ERR, "vo_pxa: src != dst, scaling not supported.\n");
        goto error;
    }

    overlay_x = (overlay_width - dst_width) / 2;
    overlay_y = (overlay_height - dst_height) / 2;

    overlay_fd = open("/dev/fb2", O_RDWR);
    r = ioctl(overlay_fd, FBIOGET_VSCREENINFO, &fbvar);
    if (r) {
        mp_msg(MSGT_VO, MSGL_ERR, "vo_pxa: get_vscreeninfo ioctl failed\n");
        goto error;
    }

    fbvar.xres_virtual = fbvar.xres = overlay_width;
    fbvar.yres_virtual = fbvar.yres = overlay_height;
    fbvar.bits_per_pixel = 16;
    fbvar.activate = FB_ACTIVATE_NOW;

    fbvar.nonstd = (4 << 20) | (0 << 10) | (0); /* YUV420P, X=0, Y=0 */

    r = ioctl(overlay_fd, FBIOPUT_VSCREENINFO, &fbvar);
    if (r) {
        mp_msg(MSGT_VO, MSGL_ERR, "vo_pxa: put_vscreeninfo ioctl failed\n");
        goto error;
    }
    r = ioctl(overlay_fd, FBIOGET_FSCREENINFO, &fbfix);
    if (r) {
        mp_msg(MSGT_VO, MSGL_ERR, "vo_pxa: get_fscreeninfo ioctl failed\n");
        goto error;
    }

    overlay_mem_size = fbfix.smem_len;
    overlay_mem = mmap(NULL, overlay_mem_size, (PROT_READ | PROT_WRITE),
        MAP_SHARED, overlay_fd, 0);

    if (overlay_mem == MAP_FAILED) {
        mp_msg(MSGT_VO, MSGL_ERR, "vo_pxa: failed to mmap overlay");
        goto error;
    }

    y_plane = overlay_mem;
    u_plane = y_plane + overlay_width * overlay_height;
    v_plane = u_plane + overlay_width * overlay_height / 4;

    /* Fill the overlay with black */
    memset(y_plane, 16,
        overlay_width * overlay_height);
    memset(u_plane, 128,
        overlay_width * overlay_height / 4);
    memset(v_plane, 128,
        overlay_width * overlay_height / 4);

    mp_msg(MSGT_VO, MSGL_V, "vo_pxa: configured and opened 2nd overlay");

    return 0;

error:
    if (overlay_mem)
        munmap(overlay_mem, overlay_mem_size);
    if (overlay_fd != -1)
        close(overlay_fd);

    overlay_mem = NULL;
    overlay_fd = -1;

    return -1;
}

static int control(uint32_t request, void *data, ...)
{
    mp_msg(MSGT_VO, MSGL_V, "vo_pxa: control %08x\n", request );

    switch( request )
    {
        case VOCTRL_QUERY_FORMAT:
            return( vo_pxa_check_format( *(uint32_t *)data ) );
            break;
    }

    return VO_NOTIMPL;
}


int draw_frame(uint8_t *src[])
{
    return 1;
}


/*****************************************************************************
 *
 * draw_slice
 *
 * Draw a planar YUV slice to the buffer:
 * params:
 *   src[3] = source image planes (Y,U,V)
 *   stride[3] = source image planes line widths (in bytes)
 *   w,h = width*height of area to be copied (in Y pixels)
 *   x,y = position at the destination image (in Y pixels)
 *
 ****************************************************************************/
int draw_slice(uint8_t *src[], int stride[], int w,int h, int x,int y)
{
    int i;

    x += overlay_x;
    y += overlay_y;

    uint8_t *dst_y = y_plane + y * overlay_width + x;
    uint8_t *dst_u = u_plane + y * overlay_width / 4 + x / 2;
    uint8_t *dst_v = v_plane + y * overlay_width / 4 + x / 2;
    uint8_t *src_y = src[0];
    uint8_t *src_u = src[1];
    uint8_t *src_v = src[2];

    mp_msg(MSGT_VO, MSGL_V, "vo_pxa: draw_slice() w %d h %d x %d y %d stride %d %d %d\n",
           w, h, x, y, stride[0], stride[1], stride[2] );

    if (w + x > overlay_width || h + y > overlay_height) {
        mp_msg(MSGT_VO, MSGL_ERR, "vo_pxa: too big image\n");
        return 0;
    }

    for (i = 0; i < h; i++) {
        fast_memcpy(dst_y, src_y, w);
        src_y += stride[0];
        dst_y += overlay_width;
    }
    for (i = 0; i < h / 2; i++) {
        fast_memcpy(dst_u, src_u, w / 2);
        fast_memcpy(dst_v, src_v, w / 2);
        src_u += stride[1];
        src_v += stride[2];
        dst_u += overlay_width / 2;
        dst_v += overlay_width / 2;
    }

    return 0;
}

static void draw_osd(void)
{
    /* FIXME */
    /* vo_draw_text(in_width, in_height, draw_alpha); */
}

static void flip_page(void)
{
}

static void check_events(void)
{
}

static void uninit(void)
{
    if (overlay_mem)
        munmap(overlay_mem, overlay_mem_size);
    if (overlay_fd != -1)
        close(overlay_fd);

    overlay_mem = NULL;
    overlay_fd = -1;
}
