/*
 * kms_paint — atomic KMS commit to replace Qualcomm cont_splash.
 */
#define _GNU_SOURCE
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <poll.h>
#include "ui.h"

#define DRM_PLANE_TYPE_PRIMARY 1
#define MAX_ATOMIC 160

static int drm_fd = -1;

static struct {
    int active;
    int atomic_ok;
    uint32_t fb_id, plane_id, crtc_id, conn_id, W, H, pitch;
    uint32_t *fb_mem;
} g_hold;

static int g_ui_mode;

static void kmsg(const char *s)
{
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0) {
        char b[128];
        int n = snprintf(b, sizeof(b), "<6>kms_paint: %s\n", s);
        if (n > 0) (void)write(fd, b, n);
        close(fd);
    }
}

static void kmsgf(const char *fmt, ...)
{
    char b[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    kmsg(b);
}

static int prop_id(uint32_t obj_id, uint32_t obj_type, const char *name)
{
    uint32_t props[64];
    uint64_t vals[64];
    struct drm_mode_obj_get_properties g = {
        .obj_id = obj_id,
        .obj_type = obj_type,
        .props_ptr = (uint64_t)(uintptr_t)props,
        .prop_values_ptr = (uint64_t)(uintptr_t)vals,
        .count_props = 64,
    };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &g) < 0)
        return -1;
    for (uint32_t i = 0; i < g.count_props; i++) {
        struct drm_mode_get_property gp = { .prop_id = props[i] };
        if (ioctl(drm_fd, DRM_IOCTL_MODE_GETPROPERTY, &gp) < 0)
            continue;
        if (!strcmp(gp.name, name))
            return (int)props[i];
    }
    return -1;
}

static int prop_val(uint32_t obj_id, uint32_t obj_type, const char *name, uint64_t *out)
{
    uint32_t props[64];
    uint64_t vals[64];
    struct drm_mode_obj_get_properties g = {
        .obj_id = obj_id,
        .obj_type = obj_type,
        .props_ptr = (uint64_t)(uintptr_t)props,
        .prop_values_ptr = (uint64_t)(uintptr_t)vals,
        .count_props = 64,
    };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &g) < 0)
        return -1;
    for (uint32_t i = 0; i < g.count_props; i++) {
        struct drm_mode_get_property gp = { .prop_id = props[i] };
        if (ioctl(drm_fd, DRM_IOCTL_MODE_GETPROPERTY, &gp) < 0)
            continue;
        if (!strcmp(gp.name, name)) {
            *out = vals[i];
            return 0;
        }
    }
    return -1;
}

struct atom {
    uint32_t objs[MAX_ATOMIC];
    uint32_t counts[MAX_ATOMIC];
    uint32_t props[MAX_ATOMIC];
    uint64_t vals[MAX_ATOMIC];
    int n_obj;
    int n_prop;
};

static void atom_reset(struct atom *a)
{
    memset(a, 0, sizeof(*a));
}

static int atom_add(struct atom *a, uint32_t obj, uint32_t prop, uint64_t val)
{
    if (a->n_prop >= MAX_ATOMIC)
        return -1;
    int oi = -1;
    for (int i = 0; i < a->n_obj; i++)
        if (a->objs[i] == obj) { oi = i; break; }
    if (oi < 0) {
        if (a->n_obj >= MAX_ATOMIC)
            return -1;
        oi = a->n_obj++;
        a->objs[oi] = obj;
        a->counts[oi] = 0;
    }
    a->props[a->n_prop] = prop;
    a->vals[a->n_prop] = val;
    a->n_prop++;
    a->counts[oi]++;
    return 0;
}

static int atom_set(struct atom *a, uint32_t obj, uint32_t prop, uint64_t val)
{
    int flat = 0;
    for (int oi = 0; oi < a->n_obj; oi++) {
        for (uint32_t j = 0; j < a->counts[oi]; j++, flat++) {
            if (a->objs[oi] == obj && a->props[flat] == prop) {
                a->vals[flat] = val;
                return 0;
            }
        }
    }
    return atom_add(a, obj, prop, val);
}

static int atom_copy_obj(struct atom *a, uint32_t obj, uint32_t type)
{
    uint32_t props[128];
    uint64_t vals[128];
    struct drm_mode_obj_get_properties g = {
        .obj_id = obj,
        .obj_type = type,
        .props_ptr = (uint64_t)(uintptr_t)props,
        .prop_values_ptr = (uint64_t)(uintptr_t)vals,
        .count_props = 128,
    };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &g) < 0)
        return -1;
    for (uint32_t i = 0; i < g.count_props; i++)
        if (atom_add(a, obj, props[i], vals[i]) < 0)
            return -1;
    return (int)g.count_props;
}

static int atom_commit(struct atom *a, uint32_t flags)
{
    struct drm_mode_atomic req = {
        .flags = flags,
        .count_objs = (uint32_t)a->n_obj,
        .objs_ptr = (uint64_t)(uintptr_t)a->objs,
        .count_props_ptr = (uint64_t)(uintptr_t)a->counts,
        .props_ptr = (uint64_t)(uintptr_t)a->props,
        .prop_values_ptr = (uint64_t)(uintptr_t)a->vals,
    };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_ATOMIC, &req) < 0)
        return -errno;
    return 0;
}

static void disable_bootsplash(void)
{
    int fd = open("/sys/module/drm/parameters/bootsplash_enabled", O_WRONLY);
    if (fd >= 0) {
        (void)write(fd, "0", 1);
        close(fd);
    }
}

static int ensure_drm_dev(void)
{
    if (access("/dev/dri/card0", F_OK) == 0)
        return 0;
    mkdir("/dev/dri", 0755);
    char buf[32];
    int fd = open("/sys/class/drm/card0/dev", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    unsigned maj = 0, min = 0;
    if (sscanf(buf, "%u:%u", &maj, &min) != 2)
        return -1;
    if (mknod("/dev/dri/card0", S_IFCHR | 0666, makedev(maj, min)) < 0 && errno != EEXIST)
        return -1;
    kmsg("mknod card0");
    return access("/dev/dri/card0", F_OK) == 0 ? 0 : -1;
}

static int find_planes_on_crtc(uint32_t crtc_id, uint32_t *out, int max_out)
{
    struct drm_mode_get_plane_res pres = {0};
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) < 0 || !pres.count_planes)
        return 0;

    uint32_t ids[32];
    pres.plane_id_ptr = (uint64_t)(uintptr_t)ids;
    pres.count_planes = pres.count_planes > 32 ? 32 : pres.count_planes;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) < 0)
        return 0;

    int n = 0;
    for (uint32_t i = 0; i < pres.count_planes && n < max_out; i++) {
        uint64_t pc = 0;
        if (prop_val(ids[i], DRM_MODE_OBJECT_PLANE, "CRTC_ID", &pc) == 0 &&
            pc == crtc_id)
            out[n++] = ids[i];
    }
    return n;
}

static int find_primary_plane(uint32_t crtc_idx, uint32_t *plane_id)
{
    struct drm_mode_get_plane_res pres = {0};
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) < 0)
        return -1;
    if (!pres.count_planes)
        return -1;

    uint32_t ids[16];
    pres.plane_id_ptr = (uint64_t)(uintptr_t)ids;
    pres.count_planes = pres.count_planes > 16 ? 16 : pres.count_planes;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) < 0)
        return -1;

    uint32_t mask = 1u << crtc_idx;
    for (uint32_t i = 0; i < pres.count_planes; i++) {
        struct drm_mode_get_plane gp = { .plane_id = ids[i] };
        if (ioctl(drm_fd, DRM_IOCTL_MODE_GETPLANE, &gp) < 0)
            continue;
        if (!(gp.possible_crtcs & mask))
            continue;

        int type_prop = prop_id(ids[i], DRM_MODE_OBJECT_PLANE, "type");
        if (type_prop >= 0) {
            uint64_t type = 0;
            if (!prop_val(ids[i], DRM_MODE_OBJECT_PLANE, "type", &type) &&
                type != DRM_PLANE_TYPE_PRIMARY)
                continue;
        }
        *plane_id = ids[i];
        return 0;
    }
    return -1;
}

static int get_connector_mode(uint32_t *conn_id, uint32_t *crtc_id,
                              struct drm_mode_modeinfo *mode, uint32_t *crtc_idx)
{
    struct drm_mode_card_res res = {0};
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
        return -1;
    if (!res.count_connectors || !res.count_crtcs)
        return -1;

    uint32_t connectors[8], crtcs[8], encoders[8];
    res.connector_id_ptr = (uint64_t)(uintptr_t)connectors;
    res.crtc_id_ptr      = (uint64_t)(uintptr_t)crtcs;
    res.encoder_id_ptr   = (uint64_t)(uintptr_t)encoders;
    res.count_connectors = res.count_connectors > 8 ? 8 : res.count_connectors;
    res.count_crtcs      = res.count_crtcs > 8 ? 8 : res.count_crtcs;
    res.count_encoders   = res.count_encoders > 8 ? 8 : res.count_encoders;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
        return -1;

    *crtc_id = crtcs[0];
    *crtc_idx = 0;

    for (uint32_t ci = 0; ci < res.count_connectors; ci++) {
        struct drm_mode_modeinfo modes[32];
        struct drm_mode_get_connector c = { .connector_id = connectors[ci] };
        uint32_t encs[4];
        uint32_t props[64];
        uint64_t prop_vals[64];

        c.modes_ptr = (uint64_t)(uintptr_t)modes;
        c.count_modes = 32;
        c.encoders_ptr = (uint64_t)(uintptr_t)encs;
        c.count_encoders = 4;
        c.props_ptr = (uint64_t)(uintptr_t)props;
        c.prop_values_ptr = (uint64_t)(uintptr_t)prop_vals;
        c.count_props = 64;
        if (ioctl(drm_fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0)
            continue;
        if (c.count_modes == 0 || c.connection == 2)
            continue;
        if (c.connector_type == DRM_MODE_CONNECTOR_VIRTUAL)
            continue;

        int mi = 0;
        for (uint32_t m = 0; m < c.count_modes; m++)
            if (modes[m].type & DRM_MODE_TYPE_PREFERRED) { mi = (int)m; break; }

        *conn_id = connectors[ci];
        *mode = modes[mi];
        *crtc_id = crtcs[0];
        *crtc_idx = 0;
        if (c.encoder_id) {
            struct drm_mode_get_encoder e = { .encoder_id = c.encoder_id };
            if (!ioctl(drm_fd, DRM_IOCTL_MODE_GETENCODER, &e) && e.crtc_id) {
                *crtc_id = e.crtc_id;
                for (uint32_t k = 0; k < res.count_crtcs; k++)
                    if (crtcs[k] == e.crtc_id) { *crtc_idx = k; break; }
            }
        }
        kmsgf("conn=%u crtc=%u %ux%u", *conn_id, *crtc_id,
              mode->hdisplay, mode->vdisplay);
        return 0;
    }
    return -1;
}

static int create_green_fb(uint32_t W, uint32_t H, uint32_t *fb_id,
                           uint32_t **mem_out, uint32_t *pitch_out)
{
    struct drm_mode_create_dumb cd = { .width = W, .height = H, .bpp = 32 };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0)
        return -1;

    struct drm_mode_fb_cmd2 fb2 = {
        .width = W, .height = H,
        .pixel_format = DRM_FORMAT_XRGB8888,
        .handles = { cd.handle },
        .pitches = { cd.pitch },
    };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_ADDFB2, &fb2) < 0)
        return -1;

    struct drm_mode_map_dumb md = { .handle = cd.handle };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0)
        return -1;

    uint32_t *mem = mmap(NULL, cd.size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, drm_fd, md.offset);
    if (mem == MAP_FAILED)
        return -1;

    uint32_t stride = cd.pitch / 4;
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++)
            mem[y * stride + x] = 0xFF0D1117;

    *fb_id = fb2.fb_id;
    *mem_out = mem;
    if (pitch_out)
        *pitch_out = cd.pitch;
    return 0;
}

static int try_page_flip(uint32_t crtc_id, uint32_t fb_id)
{
    struct drm_mode_crtc_page_flip flip = {
        .crtc_id = crtc_id,
        .fb_id = fb_id,
    };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) < 0)
        return -errno;
    return 0;
}

static int try_legacy_setcrtc(uint32_t conn_id, uint32_t crtc_id,
                              struct drm_mode_modeinfo *mode, uint32_t fb_id)
{
    struct drm_mode_crtc set = {
        .crtc_id = crtc_id,
        .fb_id = fb_id,
        .set_connectors_ptr = (uint64_t)(uintptr_t)&conn_id,
        .count_connectors = 1,
        .mode = *mode,
        .mode_valid = 1,
    };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0)
        return -errno;
    return 0;
}

static int try_atomic_handoff(uint32_t conn_id, uint32_t crtc_id,
                              struct drm_mode_modeinfo *mode)
{
    struct drm_mode_create_blob blob = {
        .length = sizeof(*mode),
        .data = (uint64_t)(uintptr_t)mode,
    };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob) < 0)
        return -errno;

    int p_cc = prop_id(conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    int p_act = prop_id(crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    int p_mode = prop_id(crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    if (p_cc < 0 || p_act < 0 || p_mode < 0)
        return -ENOENT;

    struct atom a;
    atom_reset(&a);
    atom_add(&a, conn_id, (uint32_t)p_cc, crtc_id);
    atom_add(&a, crtc_id, (uint32_t)p_act, 1);
    atom_add(&a, crtc_id, (uint32_t)p_mode, blob.blob_id);

    int r = atom_commit(&a, DRM_MODE_ATOMIC_ALLOW_MODESET);
    if (r)
        kmsgf("handoff errno=%d", -r);
    else
        usleep(200000);
    return r;
}

static int try_atomic_plane_keep(uint32_t plane_id, uint32_t crtc_id,
                                 uint32_t fb_id, uint32_t W, uint32_t H)
{
    struct atom a;
    atom_reset(&a);
    if (atom_copy_obj(&a, plane_id, DRM_MODE_OBJECT_PLANE) < 0)
        return -ENOENT;

    int p_fb = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
    int p_pc = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    int p_cw = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    int p_ch = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    int p_sw = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
    int p_sh = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");
    if (p_fb < 0)
        return -ENOENT;

    atom_set(&a, plane_id, (uint32_t)p_fb, fb_id);
    if (p_pc >= 0) atom_set(&a, plane_id, (uint32_t)p_pc, crtc_id);
    if (p_cw >= 0) atom_set(&a, plane_id, (uint32_t)p_cw, W);
    if (p_ch >= 0) atom_set(&a, plane_id, (uint32_t)p_ch, H);
    if (p_sw >= 0) atom_set(&a, plane_id, (uint32_t)p_sw, (uint64_t)W << 16);
    if (p_sh >= 0) atom_set(&a, plane_id, (uint32_t)p_sh, (uint64_t)H << 16);

    int r = atom_commit(&a, 0);
    if (r)
        kmsgf("plane_keep errno=%d", -r);
    return r;
}

/* SDE cont_splash: commit connector + crtc + every plane on that crtc. */
static int try_atomic_takeover(uint32_t conn_id, uint32_t crtc_id,
                               struct drm_mode_modeinfo *mode,
                               uint32_t fb_id, uint32_t W, uint32_t H,
                               uint32_t fallback_plane)
{
    struct drm_mode_create_blob blob = {
        .length = sizeof(*mode),
        .data = (uint64_t)(uintptr_t)mode,
    };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob) < 0)
        return -errno;

    int p_cc = prop_id(conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    int p_act = prop_id(crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    int p_mode = prop_id(crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    if (p_cc < 0 || p_act < 0 || p_mode < 0)
        return -ENOENT;

    uint32_t planes[16];
    int np = find_planes_on_crtc(crtc_id, planes, 16);
    if (np <= 0 && fallback_plane)
        planes[np++] = fallback_plane;
    if (np <= 0)
        return -ENOENT;

    struct atom a;
    atom_reset(&a);
    if (atom_copy_obj(&a, conn_id, DRM_MODE_OBJECT_CONNECTOR) < 0)
        return -EIO;
    atom_set(&a, conn_id, (uint32_t)p_cc, crtc_id);

    if (atom_copy_obj(&a, crtc_id, DRM_MODE_OBJECT_CRTC) < 0)
        return -EIO;
    atom_set(&a, crtc_id, (uint32_t)p_act, 1);
    atom_set(&a, crtc_id, (uint32_t)p_mode, blob.blob_id);

    for (int i = 0; i < np; i++) {
        uint32_t pid = planes[i];
        if (atom_copy_obj(&a, pid, DRM_MODE_OBJECT_PLANE) < 0)
            continue;

        int p_fb = prop_id(pid, DRM_MODE_OBJECT_PLANE, "FB_ID");
        int p_pc = prop_id(pid, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
        int p_cx = prop_id(pid, DRM_MODE_OBJECT_PLANE, "CRTC_X");
        int p_cy = prop_id(pid, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
        int p_cw = prop_id(pid, DRM_MODE_OBJECT_PLANE, "CRTC_W");
        int p_ch = prop_id(pid, DRM_MODE_OBJECT_PLANE, "CRTC_H");
        int p_sx = prop_id(pid, DRM_MODE_OBJECT_PLANE, "SRC_X");
        int p_sy = prop_id(pid, DRM_MODE_OBJECT_PLANE, "SRC_Y");
        int p_sw = prop_id(pid, DRM_MODE_OBJECT_PLANE, "SRC_W");
        int p_sh = prop_id(pid, DRM_MODE_OBJECT_PLANE, "SRC_H");
        if (p_fb < 0)
            continue;

        atom_set(&a, pid, (uint32_t)p_fb, fb_id);
        if (p_pc >= 0) atom_set(&a, pid, (uint32_t)p_pc, crtc_id);
        if (p_cx >= 0) atom_set(&a, pid, (uint32_t)p_cx, 0);
        if (p_cy >= 0) atom_set(&a, pid, (uint32_t)p_cy, 0);
        if (p_cw >= 0) atom_set(&a, pid, (uint32_t)p_cw, W);
        if (p_ch >= 0) atom_set(&a, pid, (uint32_t)p_ch, H);
        if (p_sx >= 0) atom_set(&a, pid, (uint32_t)p_sx, 0);
        if (p_sy >= 0) atom_set(&a, pid, (uint32_t)p_sy, 0);
        if (p_sw >= 0) atom_set(&a, pid, (uint32_t)p_sw, (uint64_t)W << 16);
        if (p_sh >= 0) atom_set(&a, pid, (uint32_t)p_sh, (uint64_t)H << 16);
    }

    int r = atom_commit(&a, DRM_MODE_ATOMIC_ALLOW_MODESET);
    if (r)
        kmsgf("takeover errno=%d planes=%d", -r, np);
    else
        kmsgf("OK takeover planes=%d", np);
    return r;
}

static int try_atomic_full(uint32_t conn_id, uint32_t crtc_id, uint32_t crtc_idx,
                           uint32_t plane_id, struct drm_mode_modeinfo *mode,
                           uint32_t fb_id, uint32_t W, uint32_t H)
{
    struct drm_mode_create_blob blob = {
        .length = sizeof(*mode),
        .data = (uint64_t)(uintptr_t)mode,
    };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob) < 0)
        return -errno;

    int p_crtc = prop_id(conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    int p_active = prop_id(crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    int p_mode = prop_id(crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    int p_fb = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
    int p_pcrtc = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    int p_cx = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    int p_cy = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    int p_cw = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    int p_ch = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    int p_sx = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
    int p_sy = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    int p_sw = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
    int p_sh = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");

    if (p_crtc < 0 || p_active < 0 || p_mode < 0 || p_fb < 0 || p_pcrtc < 0)
        return -ENOENT;

    struct atom a;
    atom_reset(&a);
    atom_add(&a, conn_id, (uint32_t)p_crtc, crtc_id);
    atom_add(&a, crtc_id, (uint32_t)p_active, 1);
    atom_add(&a, crtc_id, (uint32_t)p_mode, blob.blob_id);
    atom_add(&a, plane_id, (uint32_t)p_fb, fb_id);
    atom_add(&a, plane_id, (uint32_t)p_pcrtc, crtc_id);
    if (p_cx >= 0) atom_add(&a, plane_id, (uint32_t)p_cx, 0);
    if (p_cy >= 0) atom_add(&a, plane_id, (uint32_t)p_cy, 0);
    if (p_cw >= 0) atom_add(&a, plane_id, (uint32_t)p_cw, W);
    if (p_ch >= 0) atom_add(&a, plane_id, (uint32_t)p_ch, H);
    if (p_sx >= 0) atom_add(&a, plane_id, (uint32_t)p_sx, 0);
    if (p_sy >= 0) atom_add(&a, plane_id, (uint32_t)p_sy, 0);
    if (p_sw >= 0) atom_add(&a, plane_id, (uint32_t)p_sw, (uint64_t)W << 16);
    if (p_sh >= 0) atom_add(&a, plane_id, (uint32_t)p_sh, (uint64_t)H << 16);

    (void)crtc_idx;
    int r = atom_commit(&a, DRM_MODE_ATOMIC_ALLOW_MODESET);
    if (r)
        kmsgf("atomic full errno=%d", -r);
    return r;
}

static int try_atomic_plane_only(uint32_t plane_id, uint32_t crtc_id,
                                 uint32_t fb_id, uint32_t W, uint32_t H)
{
    int p_fb = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
    int p_pcrtc = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    int p_cw = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    int p_ch = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    int p_sw = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
    int p_sh = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");
    if (p_fb < 0)
        return -ENOENT;

    struct atom a;
    atom_reset(&a);
    atom_add(&a, plane_id, (uint32_t)p_fb, fb_id);
    if (p_pcrtc >= 0) atom_add(&a, plane_id, (uint32_t)p_pcrtc, crtc_id);
    if (p_cw >= 0) atom_add(&a, plane_id, (uint32_t)p_cw, W);
    if (p_ch >= 0) atom_add(&a, plane_id, (uint32_t)p_ch, H);
    if (p_sw >= 0) atom_add(&a, plane_id, (uint32_t)p_sw, (uint64_t)W << 16);
    if (p_sh >= 0) atom_add(&a, plane_id, (uint32_t)p_sh, (uint64_t)H << 16);

    int r = atom_commit(&a, 0);
    if (r)
        kmsgf("atomic plane errno=%d", -r);
    return r;
}

static void wf(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd >= 0) { write(fd, val, strlen(val)); close(fd); }
}

static void unblank(void)
{
    wf("/sys/class/graphics/fb0/blank", "0");

    DIR *d = opendir("/sys/class/backlight");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[128], maxv[16] = "255";
        snprintf(p, sizeof(p), "/sys/class/backlight/%s/max_brightness", e->d_name);
        int fd = open(p, O_RDONLY);
        if (fd >= 0) {
            int n = read(fd, maxv, sizeof(maxv) - 1);
            close(fd);
            if (n > 0) {
                maxv[n] = '\0';
                while (n > 0 && (maxv[n-1] == '\n' || maxv[n-1] == ' '))
                    maxv[--n] = '\0';
            }
        }
        snprintf(p, sizeof(p), "/sys/class/backlight/%s/brightness", e->d_name);
        fd = open(p, O_WRONLY);
        if (fd >= 0) { write(fd, maxv, strlen(maxv)); close(fd); }
        snprintf(p, sizeof(p), "/sys/class/backlight/%s/bl_power", e->d_name);
        fd = open(p, O_WRONLY);
        if (fd >= 0) { write(fd, "0", 1); close(fd); }
    }
    closedir(d);
}

static void dpms_on(uint32_t conn_id)
{
    int p = prop_id(conn_id, DRM_MODE_OBJECT_CONNECTOR, "DPMS");
    if (p < 0)
        return;
    struct atom a;
    atom_reset(&a);
    atom_add(&a, conn_id, (uint32_t)p, 0);
    atom_commit(&a, 0);
}

static void mark_success(uint32_t conn_id, uint32_t crtc_id, uint32_t plane_id,
                         uint32_t fb_id, uint32_t W, uint32_t H, int atomic_ok,
                         uint32_t *fb_mem, uint32_t pitch)
{
    g_hold.active = 1;
    g_hold.atomic_ok = atomic_ok;
    g_hold.conn_id = conn_id;
    g_hold.crtc_id = crtc_id;
    g_hold.plane_id = plane_id;
    g_hold.fb_id = fb_id;
    g_hold.W = W;
    g_hold.H = H;
    g_hold.pitch = pitch;
    g_hold.fb_mem = fb_mem;
}

static void hold_forever(void)
{
    if (!g_hold.active)
        return;
    kmsg(g_ui_mode ? "hold ui" : "hold start");
    dpms_on(g_hold.conn_id);

    UiDrm uctx = {
        .fb_mem = g_hold.fb_mem,
        .fb_id = g_hold.fb_id,
        .w = g_hold.W,
        .h = g_hold.H,
        .pitch = g_hold.pitch,
        .plane_id = g_hold.plane_id,
        .crtc_id = g_hold.crtc_id,
        .conn_id = g_hold.conn_id,
        .atomic_ok = g_hold.atomic_ok,
    };

    if (g_ui_mode && g_hold.fb_mem) {
        ui_init(&uctx);
        /* Plane already points at our dumb FB — CPU writes scan out via SDE. */
        kmsg("hold ui scanout");
    } else if (g_hold.atomic_ok && g_hold.plane_id) {
        try_atomic_plane_keep(g_hold.plane_id, g_hold.crtc_id,
                              g_hold.fb_id, g_hold.W, g_hold.H);
    }

    unblank();
    disable_bootsplash();

    for (int n = 0; ; n++) {
        if (g_ui_mode && g_hold.fb_mem) {
            int ms = ui_tick(&uctx);
            if (ms < 50)
                ms = 50;
            struct pollfd pfd = { .fd = ui_touch_fd(), .events = POLLIN };
            if (pfd.fd >= 0)
                poll(&pfd, 1, ms);
            else
                usleep((useconds_t)ms * 1000U);
            if (n % 6000 == 5999)
                unblank();
            continue;
        }

        unblank();
        disable_bootsplash();
        if (g_hold.atomic_ok && g_hold.plane_id && n % 120 == 0)
            try_atomic_plane_keep(g_hold.plane_id, g_hold.crtc_id,
                                  g_hold.fb_id, g_hold.W, g_hold.H);
        if (n % 20 == 0) {
            struct drm_mode_fb_dirty_cmd dirty = { .fb_id = g_hold.fb_id };
            ioctl(drm_fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
        }
        if (n % 200 == 0)
            kmsg("hold");
        usleep(500000);
    }
}

int paint_once(void)
{
    prctl(PR_SET_NAME, "recovery", 0, 0, 0);

    ensure_drm_dev();
    if (access("/dev/dri/card0", F_OK) != 0)
        return 1;

    disable_bootsplash();

    if (drm_fd < 0) {
        drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
        if (drm_fd < 0) { kmsg("open fail"); return 1; }
        ioctl(drm_fd, DRM_IOCTL_SET_MASTER, NULL);
        struct drm_auth auth = {0};
        if (!ioctl(drm_fd, DRM_IOCTL_GET_MAGIC, &auth))
            ioctl(drm_fd, DRM_IOCTL_AUTH_MAGIC, &auth);
        ioctl(drm_fd, DRM_IOCTL_SET_MASTER, NULL);
        struct drm_set_client_cap cap = { .capability = DRM_CLIENT_CAP_ATOMIC, .value = 1 };
        if (ioctl(drm_fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) < 0)
            kmsg("no atomic cap");
        cap.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES;
        cap.value = 1;
        ioctl(drm_fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
    }

    uint32_t conn_id = 0, crtc_id = 0, crtc_idx = 0, plane_id = 0, fb_id = 0;
    struct drm_mode_modeinfo mode = {0};
    if (get_connector_mode(&conn_id, &crtc_id, &mode, &crtc_idx) < 0) {
        kmsg("no mode");
        return 3;
    }

    int atomic_ok = (prop_id(conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID") >= 0);
    if (find_primary_plane(crtc_idx, &plane_id) < 0)
        kmsg("no plane");

    struct drm_mode_crtc gc = { .crtc_id = crtc_id };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETCRTC, &gc) == 0)
        kmsgf("crtc fb=%u %ux%u valid=%u", gc.fb_id,
              gc.mode.hdisplay, gc.mode.vdisplay, gc.mode_valid);

    uint32_t W = mode.hdisplay, H = mode.vdisplay;
    uint32_t *mem = NULL;
    uint32_t pitch = 0;
    if (create_green_fb(W, H, &fb_id, &mem, &pitch) < 0) {
        kmsg("fb fail");
        return 4;
    }

    int r;

    /* Safest first: swap FB on existing plane (no modeset). */
    if (atomic_ok && plane_id) {
        r = try_atomic_plane_keep(plane_id, crtc_id, fb_id, W, H);
        if (!r) {
            kmsg("OK plane_keep");
            mark_success(conn_id, crtc_id, plane_id, fb_id, W, H, atomic_ok, mem, pitch);
            unblank();
            return 0;
        }
    }

    if (atomic_ok && plane_id) {
        r = try_atomic_plane_only(plane_id, crtc_id, fb_id, W, H);
        if (!r) {
            kmsg("OK plane_only");
            mark_success(conn_id, crtc_id, plane_id, fb_id, W, H, atomic_ok, mem, pitch);
            unblank();
            return 0;
        }
    }

    if (atomic_ok) {
        r = try_atomic_handoff(conn_id, crtc_id, &mode);
        if (!r && plane_id) {
            r = try_atomic_plane_keep(plane_id, crtc_id, fb_id, W, H);
            if (!r) {
                kmsg("OK handoff+plane");
                mark_success(conn_id, crtc_id, plane_id, fb_id, W, H, atomic_ok, mem, pitch);
                unblank();
                return 0;
            }
        }
    }

    /* Full modeset last — can stress cont_splash / SDE. */
    if (atomic_ok) {
        r = try_atomic_takeover(conn_id, crtc_id, &mode, fb_id, W, H, plane_id);
        if (!r) {
            usleep(150000);
            kmsg("OK takeover");
            mark_success(conn_id, crtc_id, plane_id, fb_id, W, H, atomic_ok, mem, pitch);
            unblank();
            return 0;
        }
    }

    if (atomic_ok && plane_id) {
        r = try_atomic_full(conn_id, crtc_id, crtc_idx, plane_id, &mode, fb_id, W, H);
        if (!r) {
            kmsg("OK atomic full");
            mark_success(conn_id, crtc_id, plane_id, fb_id, W, H, atomic_ok, mem, pitch);
            unblank();
            return 0;
        }
    }

    kmsg("all atomic paths failed");
    return 9;
}

int main(int argc, char **argv)
{
    int quick = 0, hold = 0;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--quick"))
            quick = 1;
        if (!strcmp(argv[a], "--hold"))
            hold = 1;
        if (!strcmp(argv[a], "--ui"))
            g_ui_mode = 1;
    }

    kmsg(g_ui_mode ? (hold ? "start ui hold" : "start ui") :
                     (hold ? "start hold" : "start"));
    int max = quick ? 120 : 40;
    for (int i = 0; i < max; i++) {
        int r = paint_once();
        if (r == 0) {
            if (hold)
                hold_forever();
            return 0;
        }
        usleep(r == 3 ? 400000 : 250000);
    }
    kmsg("give up");
    return 9;
}
