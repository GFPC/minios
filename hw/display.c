#define _GNU_SOURCE
#include <dirent.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>
#include "minios/display.h"
#include "minios/log.h"
#include "minios/sysfs.h"

int   fb_fd   = -1;
unsigned int *fb_mem  = NULL;
size_t        fb_sz   = 0;
unsigned int  fb_W    = 0, fb_H = 0, fb_stride = 0;

void fb_open(void)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;

    const char *fbdev = NULL;
    if (access("/dev/fb0", F_OK) == 0) fbdev = "/dev/fb0";
    else if (access("/dev/graphics/fb0", F_OK) == 0) fbdev = "/dev/graphics/fb0";
    if (!fbdev) return;
    fb_fd = open(fbdev, O_RDWR | O_CLOEXEC);
    if (fb_fd < 0) { klogf("display: open %s failed", fbdev); return; }
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        klog("display: ioctl err"); close(fb_fd); fb_fd = -1; return;
    }
    fb_sz     = fix.smem_len ? fix.smem_len
                              : (size_t)var.yres_virtual * fix.line_length;
    fb_mem    = mmap(NULL, fb_sz, PROT_READ|PROT_WRITE, MAP_SHARED, fb_fd, 0);
    fb_W      = var.xres;
    fb_H      = var.yres;
    fb_stride = fix.line_length / 4;
    if (fb_mem == MAP_FAILED) {
        klog("display: mmap err"); fb_mem = NULL;
        close(fb_fd); fb_fd = -1;
    } else {
        klogf("display: fb0 %ux%u ok", fb_W, fb_H);
    }
}

/* Fill top STATUS_H rows with color, rest stays (or fills BG) */
#define STATUS_H 220
void fb_status(unsigned int color, unsigned int bg)
{
    if (!fb_mem) return;
    for (unsigned y = 0; y < fb_H; y++) {
        unsigned int c = (y < STATUS_H) ? color : bg;
        for (unsigned x = 0; x < fb_W; x++)
            fb_mem[y * fb_stride + x] = c;
    }
    /* white stripe in status area so you can tell it from logo */
    unsigned stripe_y = STATUS_H - 8;
    for (unsigned y = stripe_y; y < stripe_y + 4 && y < fb_H; y++)
        for (unsigned x = fb_W/4; x < fb_W*3/4; x++)
            fb_mem[y * fb_stride + x] = 0xffffffff;

    msync(fb_mem, fb_sz, MS_SYNC);
    sysfs_write("/sys/class/graphics/fb0/blank", "0");
}

void display_unblank(void)
{
    sysfs_write("/sys/class/graphics/fb0/blank", "0");
    DIR *d = opendir("/sys/class/backlight");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[160], maxv[32] = "255";
        snprintf(p, sizeof(p), "/sys/class/backlight/%s/max_brightness", e->d_name);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            int n = read(fd, maxv, sizeof(maxv) - 1);
            close(fd);
            if (n > 0) { maxv[n] = '\0'; while (n > 0 && (maxv[n-1]=='\n'||maxv[n-1]=='\r')) maxv[--n]='\0'; }
        }
        snprintf(p, sizeof(p), "/sys/class/backlight/%s/brightness", e->d_name);
        sysfs_write(p, maxv);
        klogf("display: backlight %s=%s", e->d_name, maxv);
    }
    closedir(d);
}

/* ── DRM display ── */
int drm_fd = -1;
uint32_t *drm_fb_mem = NULL;
size_t    drm_fb_sz  = 0;
uint32_t  drm_fb_id  = 0;
uint32_t  drm_crtc_id= 0;
uint32_t  drm_conn_id= 0;
struct drm_mode_modeinfo drm_mode;
uint32_t  drm_W = 0, drm_H = 0, drm_pitch = 0;

void drm_open(void)
{
    if (drm_fb_mem) return;
    if (access("/dev/dri/card0", F_OK) != 0) return;
    drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) { klog("DRM: open failed"); return; }

    prctl(PR_SET_NAME, "recovery", 0, 0, 0);
    ioctl(drm_fd, DRM_IOCTL_SET_MASTER, 0);
    sysfs_write("/sys/module/drm/parameters/bootsplash_enabled", "0");

    /* get resources */
    struct drm_mode_card_res res = {0};
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        klog("DRM: GETRESOURCES failed"); close(drm_fd); drm_fd=-1; return;
    }
    if (!res.count_connectors || !res.count_crtcs) {
        klog("DRM: no connectors/crtcs"); close(drm_fd); drm_fd=-1; return;
    }
    klogf("DRM: %u conn, %u crtc, %u enc",
          res.count_connectors, res.count_crtcs, res.count_encoders);

    uint32_t connectors[8], crtcs[8], encoders[8];
    res.connector_id_ptr = (uint64_t)(uintptr_t)connectors;
    res.crtc_id_ptr      = (uint64_t)(uintptr_t)crtcs;
    res.encoder_id_ptr   = (uint64_t)(uintptr_t)encoders;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        klog("DRM: GETRESOURCES2 failed"); close(drm_fd); drm_fd=-1; return;
    }

    /* find first connected connector */
    int found_conn = 0;
    for (uint32_t ci = 0; ci < res.count_connectors && ci < 8; ci++) {
        struct drm_mode_get_connector conn = {0};
        conn.connector_id = connectors[ci];
        if (ioctl(drm_fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) continue;
        if (conn.count_modes == 0) continue;
        /* accept connected or unknown — early boot often reports unknown */
        if (conn.connection == 2) continue; /* disconnected */

        struct drm_mode_modeinfo modes[16];
        conn.modes_ptr = (uint64_t)(uintptr_t)modes;
        conn.count_modes = conn.count_modes > 16 ? 16 : conn.count_modes;
        uint32_t enc_arr[4];
        uint32_t props[64];
        uint64_t prop_vals[64];
        conn.encoders_ptr = (uint64_t)(uintptr_t)enc_arr;
        conn.count_encoders = conn.count_encoders > 4 ? 4 : conn.count_encoders;
        conn.props_ptr = (uint64_t)(uintptr_t)props;
        conn.prop_values_ptr = (uint64_t)(uintptr_t)prop_vals;
        conn.count_props = 64;
        if (ioctl(drm_fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) continue;

        int mi = 0;
        for (unsigned m = 0; m < conn.count_modes && m < 16; m++)
            if (modes[m].type & DRM_MODE_TYPE_PREFERRED) { mi = (int)m; break; }

        drm_conn_id = connectors[ci];
        drm_mode    = modes[mi];
        drm_W       = drm_mode.hdisplay;
        drm_H       = drm_mode.vdisplay;

        /* find encoder → crtc */
        uint32_t enc_id = conn.encoder_id;
        if (enc_id == 0 && conn.count_encoders > 0) enc_id = enc_arr[0];
        drm_crtc_id = crtcs[0];
        if (enc_id) {
            struct drm_mode_get_encoder enc = {.encoder_id=enc_id};
            if (ioctl(drm_fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0 && enc.crtc_id)
                drm_crtc_id = enc.crtc_id;
        }
        found_conn = 1;
        break;
    }
    if (!found_conn) {
        klog("DRM: no connected display"); close(drm_fd); drm_fd=-1; return;
    }
    klogf("DRM: %ux%u conn=%u crtc=%u", drm_W, drm_H, drm_conn_id, drm_crtc_id);

    /* create dumb framebuffer */
    struct drm_mode_create_dumb create = {.width=drm_W, .height=drm_H, .bpp=32};
    if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        klog("DRM: CREATE_DUMB failed"); close(drm_fd); drm_fd=-1; return;
    }
    drm_pitch  = create.pitch;
    drm_fb_sz  = create.size;

    struct drm_mode_fb_cmd2 fb2 = {
        .width = drm_W, .height = drm_H,
        .pixel_format = DRM_FORMAT_XRGB8888,
        .handles = { create.handle },
        .pitches = { create.pitch },
    };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_ADDFB2, &fb2) < 0) {
        klogf("DRM: ADDFB2 errno=%d", errno);
        close(drm_fd); drm_fd=-1; return;
    }
    drm_fb_id = fb2.fb_id;

    struct drm_mode_map_dumb map_dumb = {.handle=create.handle};
    if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb) < 0) {
        klog("DRM: MAP_DUMB failed"); close(drm_fd); drm_fd=-1; return;
    }
    drm_fb_mem = mmap(NULL, drm_fb_sz, PROT_READ|PROT_WRITE,
                      MAP_SHARED, drm_fd, map_dumb.offset);
    if (drm_fb_mem == MAP_FAILED) {
        klog("DRM: mmap failed"); drm_fb_mem=NULL; close(drm_fd); drm_fd=-1; return;
    }

    /* activate */
    struct drm_mode_crtc set_crtc = {
        .crtc_id        = drm_crtc_id,
        .fb_id          = drm_fb_id,
        .x=0, .y=0,
        .set_connectors_ptr = (uint64_t)(uintptr_t)&drm_conn_id,
        .count_connectors   = 1,
        .mode           = drm_mode,
        .mode_valid     = 1
    };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_SETCRTC, &set_crtc) < 0) {
        klogf("DRM: SETCRTC failed errno=%d", errno);
        munmap(drm_fb_mem, drm_fb_sz); drm_fb_mem=NULL;
        close(drm_fd); drm_fd=-1; return;
    }
    display_unblank();
    struct drm_mode_fb_dirty_cmd dirty = { .fb_id = drm_fb_id };
    ioctl(drm_fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
    klogf("DRM: framebuffer active %ux%u pitch=%u", drm_W, drm_H, drm_pitch);
}

void drm_fill(uint32_t color)
{
    if (!drm_fb_mem) return;
    uint32_t stride = drm_pitch / 4;
    for (uint32_t y = 0; y < drm_H; y++)
        for (uint32_t x = 0; x < drm_W; x++)
            drm_fb_mem[y * stride + x] = color;
}

/* Draw a colored bar + text row (simplified: just solid color blocks) */
#define BAR_H 200
void drm_status(uint32_t top_color, uint32_t bg_color)
{
    if (!drm_fb_mem) return;
    uint32_t stride = drm_pitch / 4;
    for (uint32_t y = 0; y < drm_H; y++) {
        uint32_t c = (y < BAR_H) ? top_color : bg_color;
        for (uint32_t x = 0; x < drm_W; x++)
            drm_fb_mem[y * stride + x] = c;
    }
    for (uint32_t y = BAR_H - 6; y < BAR_H && y < drm_H; y++)
        for (uint32_t x = drm_W/4; x < drm_W*3/4; x++)
            drm_fb_mem[y * stride + x] = 0xFFFFFFFF;
    if (drm_fd >= 0) {
        struct drm_mode_fb_dirty_cmd dirty = { .fb_id = drm_fb_id };
        ioctl(drm_fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
    }
}

void display_status(unsigned int top, unsigned int bg)
{
    if (fb_mem)      fb_status(top, bg);
    else if (drm_fb_mem) drm_status(top, bg);
}
