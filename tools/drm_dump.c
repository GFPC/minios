/*
 * drm_dump — dump SDE/DRM topology and object properties (for trace/replay).
 */
#define _GNU_SOURCE
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <unistd.h>

static int drm_fd = -1;

static int prop_name(uint32_t pid, char *name, size_t namelen)
{
    struct drm_mode_get_property gp = { .prop_id = pid };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETPROPERTY, &gp) < 0)
        return -1;
    snprintf(name, namelen, "%s", gp.name);
    return 0;
}

static void dump_obj_props(uint32_t obj_id, uint32_t obj_type, const char *label)
{
    uint32_t props[128];
    uint64_t vals[128];
    struct drm_mode_obj_get_properties g = {
        .obj_id = obj_id,
        .obj_type = obj_type,
        .props_ptr = (uint64_t)(uintptr_t)props,
        .prop_values_ptr = (uint64_t)(uintptr_t)vals,
        .count_props = 128,
    };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &g) < 0) {
        printf("  %s props: errno=%d\n", label, errno);
        return;
    }
    printf("  %s props (%u):\n", label, g.count_props);
    for (uint32_t i = 0; i < g.count_props; i++) {
        char nm[DRM_PROP_NAME_LEN + 1] = "?";
        prop_name(props[i], nm, sizeof(nm));
        printf("    %-24s id=%-4u val=%" PRIu64 "\n", nm, props[i], vals[i]);
    }
}

static const char *conn_type_name(uint32_t t)
{
    switch (t) {
    case DRM_MODE_CONNECTOR_Unknown: return "Unknown";
    case DRM_MODE_CONNECTOR_VGA: return "VGA";
    case DRM_MODE_CONNECTOR_DVII: return "DVI-I";
    case DRM_MODE_CONNECTOR_DVID: return "DVI-D";
    case DRM_MODE_CONNECTOR_DVIA: return "DVI-A";
    case DRM_MODE_CONNECTOR_Composite: return "Composite";
    case DRM_MODE_CONNECTOR_SVIDEO: return "S-Video";
    case DRM_MODE_CONNECTOR_LVDS: return "LVDS";
    case DRM_MODE_CONNECTOR_Component: return "Component";
    case DRM_MODE_CONNECTOR_9PinDIN: return "9PinDIN";
    case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
    case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
    case DRM_MODE_CONNECTOR_TV: return "TV";
    case DRM_MODE_CONNECTOR_eDP: return "eDP";
    case DRM_MODE_CONNECTOR_VIRTUAL: return "Virtual";
    case DRM_MODE_CONNECTOR_DSI: return "DSI";
    case DRM_MODE_CONNECTOR_DPI: return "DPI";
    default: return "other";
    }
}

int main(int argc, char **argv)
{
    const char *tag = (argc > 1) ? argv[1] : "dump";
    prctl(PR_SET_NAME, "recovery", 0, 0, 0);

    drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        fprintf(stderr, "open card0: %s\n", strerror(errno));
        return 1;
    }

    ioctl(drm_fd, DRM_IOCTL_SET_MASTER, NULL);

    struct drm_set_client_cap cap = { .capability = DRM_CLIENT_CAP_ATOMIC, .value = 1 };
    int atomic = (ioctl(drm_fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) == 0);
    cap.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES;
    cap.value = 1;
    int planes = (ioctl(drm_fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) == 0);

    printf("# drm_dump tag=%s atomic=%d universal_planes=%d\n", tag, atomic, planes);

    struct drm_mode_card_res res = {0};
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        fprintf(stderr, "GETRESOURCES: %s\n", strerror(errno));
        return 1;
    }

    uint32_t connectors[16], crtcs[16], encoders[16];
    res.connector_id_ptr = (uint64_t)(uintptr_t)connectors;
    res.crtc_id_ptr      = (uint64_t)(uintptr_t)crtcs;
    res.encoder_id_ptr   = (uint64_t)(uintptr_t)encoders;
    res.count_connectors = res.count_connectors > 16 ? 16 : res.count_connectors;
    res.count_crtcs      = res.count_crtcs > 16 ? 16 : res.count_crtcs;
    res.count_encoders   = res.count_encoders > 16 ? 16 : res.count_encoders;
    ioctl(drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res);

    printf("resources: fb=%u conn=%u enc=%u crtc=%u min=%ux%u max=%ux%u\n",
           res.count_fbs, res.count_connectors, res.count_encoders, res.count_crtcs,
           res.min_width, res.min_height, res.max_width, res.max_height);

    for (uint32_t ci = 0; ci < res.count_connectors; ci++) {
        struct drm_mode_modeinfo modes[32];
        struct drm_mode_get_connector c = { .connector_id = connectors[ci] };
        uint32_t encs[8], props[128];
        uint64_t pvals[128];
        c.modes_ptr = (uint64_t)(uintptr_t)modes;
        c.count_modes = 32;
        c.encoders_ptr = (uint64_t)(uintptr_t)encs;
        c.count_encoders = 8;
        c.props_ptr = (uint64_t)(uintptr_t)props;
        c.prop_values_ptr = (uint64_t)(uintptr_t)pvals;
        c.count_props = 128;
        if (ioctl(drm_fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0)
            continue;

        printf("\nconnector %u type=%s(%u) conn=%u enc_id=%u modes=%u\n",
               connectors[ci], conn_type_name(c.connector_type), c.connector_type,
               c.connection, c.encoder_id, c.count_modes);
        for (uint32_t m = 0; m < c.count_modes && m < 8; m++)
            printf("  mode[%u] %ux%u@%u%s\n", m,
                   modes[m].hdisplay, modes[m].vdisplay, modes[m].vrefresh,
                   (modes[m].type & DRM_MODE_TYPE_PREFERRED) ? " preferred" : "");
        dump_obj_props(connectors[ci], DRM_MODE_OBJECT_CONNECTOR, "connector");
    }

    for (uint32_t i = 0; i < res.count_crtcs; i++) {
        struct drm_mode_crtc crtc = { .crtc_id = crtcs[i] };
        if (ioctl(drm_fd, DRM_IOCTL_MODE_GETCRTC, &crtc) == 0)
            printf("\ncrtc %u fb=%u x=%u y=%u mode_valid=%u %ux%u\n",
                   crtcs[i], crtc.fb_id, crtc.x, crtc.y, crtc.mode_valid,
                   crtc.mode.hdisplay, crtc.mode.vdisplay);
        dump_obj_props(crtcs[i], DRM_MODE_OBJECT_CRTC, "crtc");
    }

    struct drm_mode_get_plane_res pres = {0};
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) == 0 && pres.count_planes) {
        uint32_t pids[32];
        pres.plane_id_ptr = (uint64_t)(uintptr_t)pids;
        pres.count_planes = pres.count_planes > 32 ? 32 : pres.count_planes;
        ioctl(drm_fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres);
        printf("\nplanes total=%u\n", pres.count_planes);
        for (uint32_t i = 0; i < pres.count_planes; i++) {
            struct drm_mode_get_plane gp = { .plane_id = pids[i] };
            if (ioctl(drm_fd, DRM_IOCTL_MODE_GETPLANE, &gp) < 0)
                continue;
            printf("plane %u fb=%u crtc=%u possible_crtcs=0x%x\n",
                   pids[i], gp.fb_id, gp.crtc_id, gp.possible_crtcs);
            dump_obj_props(pids[i], DRM_MODE_OBJECT_PLANE, "plane");
        }
    }

    printf("\n# end %s\n", tag);
    return 0;
}
