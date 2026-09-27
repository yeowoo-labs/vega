#define _DEFAULT_SOURCE
#include "vega.h"
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* A display through KMS (the kernel's mode setting), with no library between:
 * a connector's mode on a CRTC, its primary plane showing one of two dumb
 * buffers (0xRRGGBB pixels the surface draws into), flipped by atomic
 * commits. On a writeback connector (a display controller's output written
 * back to memory) each shown frame is captured, so the whole path can be
 * checked with no screen attached. */

typedef struct {
    uint32_t handle, id, pitch; /* dumb buffer, framebuffer */
    uint64_t size;
    uint32_t *pixels;
} Buffer;

struct VegaDisplay {
    int fd;
    uint32_t connector, crtc, plane, mode_blob;
    struct drm_mode_modeinfo mode;
    bool writeback, set;
    Buffer buffers[2], capture;
    unsigned back;
    /* Frames are drawn here, in cached memory, then copied to the buffer
     * shown: dumb buffers are write-combined, and blending reads what it
     * draws over, which such memory makes slow. */
    uint32_t *shadow;
    /* property ids */
    uint32_t connector_crtc, crtc_mode, crtc_active, plane_fb, plane_crtc, plane_src[4], plane_dst[4], writeback_fb,
        writeback_fence;
};

enum { PRIMARY = 1 }; /* the value of a plane's "type" property for a primary plane */

static int io(int fd, unsigned long request, void *arg) {
    int r;
    do { r = ioctl(fd, request, arg); } while (r && (errno == EINTR || errno == EAGAIN));
    return r;
}

/* The id of an object's property by name, and its value. */
static uint32_t property(int fd, uint32_t object, uint32_t type, const char *name, uint64_t *value) {
    struct drm_mode_obj_get_properties list = {.obj_id = object, .obj_type = type};
    if (io(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &list) || !list.count_props) return 0;
    uint32_t *ids = calloc(list.count_props, sizeof(*ids));
    uint64_t *values = calloc(list.count_props, sizeof(*values));
    uint32_t found = 0;
    list.props_ptr = (uintptr_t)ids;
    list.prop_values_ptr = (uintptr_t)values;
    if (ids && values && !io(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &list))
        for (uint32_t i = 0; i < list.count_props && !found; ++i) {
            struct drm_mode_get_property p = {.prop_id = ids[i]};
            if (!io(fd, DRM_IOCTL_MODE_GETPROPERTY, &p) && !strcmp(p.name, name)) {
                found = ids[i];
                if (value) *value = values[i];
            }
        }
    free(ids);
    free(values);
    return found;
}

static bool make_buffer(int fd, unsigned width, unsigned height, Buffer *out) {
    struct drm_mode_create_dumb create = {.width = width, .height = height, .bpp = 32};
    if (io(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create)) return false;
    *out = (Buffer){.handle = create.handle, .pitch = create.pitch, .size = create.size};
    struct drm_mode_fb_cmd2 fb = {.width = width, .height = height, .pixel_format = DRM_FORMAT_XRGB8888,
                                  .handles = {create.handle}, .pitches = {create.pitch}};
    struct drm_mode_map_dumb map = {.handle = create.handle};
    if (io(fd, DRM_IOCTL_MODE_ADDFB2, &fb) || io(fd, DRM_IOCTL_MODE_MAP_DUMB, &map)) return false;
    out->id = fb.fb_id;
    void *pixels = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)map.offset);
    if (pixels == MAP_FAILED) return false;
    out->pixels = pixels;
    return true;
}

static void free_buffer(int fd, Buffer *buffer) {
    if (buffer->pixels) munmap(buffer->pixels, buffer->size);
    if (buffer->id) io(fd, DRM_IOCTL_MODE_RMFB, &buffer->id);
    if (buffer->handle) {
        struct drm_mode_destroy_dumb destroy = {.handle = buffer->handle};
        io(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
    *buffer = (Buffer){0};
}

#define FAIL(...) do { snprintf(error, error_size, __VA_ARGS__); vega_display_close(d); return NULL; } while (0)

VegaDisplay *vega_display_open(const char *card, bool writeback, unsigned width, unsigned height, char *error,
                               size_t error_size) {
    VegaDisplay *d = calloc(1, sizeof(*d));
    if (!d) { snprintf(error, error_size, "out of memory"); return NULL; }
    d->fd = open(card, O_RDWR | O_CLOEXEC);
    d->writeback = writeback;
    if (d->fd < 0) FAIL("cannot open %s: %s", card, strerror(errno));
    struct drm_set_client_cap caps[] = {{DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1}, {DRM_CLIENT_CAP_ATOMIC, 1},
                                        {DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1}};
    for (unsigned i = 0; i < (writeback ? 3u : 2u); ++i)
        if (io(d->fd, DRM_IOCTL_SET_CLIENT_CAP, &caps[i])) FAIL("%s has no atomic mode setting", card);
    struct drm_mode_card_res res = {0};
    if (io(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) FAIL("%s sets no modes", card);
    uint32_t crtcs[16] = {0}, connectors[16] = {0}, encoders[16] = {0};
    if (res.count_crtcs > 16 || res.count_connectors > 16 || res.count_encoders > 16) FAIL("too many outputs");
    res = (struct drm_mode_card_res){.crtc_id_ptr = (uintptr_t)crtcs, .connector_id_ptr = (uintptr_t)connectors,
                                     .encoder_id_ptr = (uintptr_t)encoders, .count_crtcs = res.count_crtcs,
                                     .count_connectors = res.count_connectors, .count_encoders = res.count_encoders};
    if (io(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) FAIL("%s sets no modes", card);
    /* The first connected connector of the kind asked for, its preferred (or
     * asked) mode, and a CRTC its encoder can drive. */
    uint32_t possible = 0;
    for (uint32_t i = 0; i < res.count_connectors && !d->connector; ++i) {
        struct drm_mode_modeinfo modes[64];
        struct drm_mode_get_connector c = {.connector_id = connectors[i]};
        if (io(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &c)) continue;
        bool back = c.connector_type == DRM_MODE_CONNECTOR_WRITEBACK;
        if (back != writeback || c.connection != 1 || !c.count_modes || c.count_modes > 64) continue;
        uint32_t encoder_ids[8] = {0};
        struct drm_mode_get_connector full = {.connector_id = connectors[i], .modes_ptr = (uintptr_t)modes,
                                              .count_modes = c.count_modes, .encoders_ptr = (uintptr_t)encoder_ids,
                                              .count_encoders = c.count_encoders < 8 ? c.count_encoders : 8};
        if (io(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &full) || !full.count_encoders) continue;
        int chosen = -1;
        for (uint32_t m = 0; m < full.count_modes && chosen < 0; ++m)
            if (width ? modes[m].hdisplay == width && modes[m].vdisplay == height : (modes[m].type & DRM_MODE_TYPE_PREFERRED))
                chosen = (int)m;
        if (chosen < 0 && !width) chosen = 0;
        if (chosen < 0) continue;
        struct drm_mode_get_encoder e = {.encoder_id = encoder_ids[0]};
        if (io(d->fd, DRM_IOCTL_MODE_GETENCODER, &e)) continue;
        d->connector = connectors[i];
        d->mode = modes[chosen];
        possible = e.possible_crtcs;
    }
    if (!d->connector) FAIL(writeback ? "no writeback connector with a %ux%u mode" : "no display connected (%ux%u)", width, height);
    for (uint32_t i = 0; i < res.count_crtcs && !d->crtc; ++i)
        if (possible & (1u << i)) d->crtc = crtcs[i];
    /* The CRTC's primary plane. */
    struct drm_mode_get_plane_res planes = {0};
    uint32_t plane_ids[64] = {0};
    if (io(d->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &planes) || planes.count_planes > 64) FAIL("no planes");
    planes.plane_id_ptr = (uintptr_t)plane_ids;
    if (io(d->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &planes)) FAIL("no planes");
    int crtc_index = -1;
    for (uint32_t i = 0; i < res.count_crtcs; ++i) if (crtcs[i] == d->crtc) crtc_index = (int)i;
    for (uint32_t i = 0; i < planes.count_planes && !d->plane && crtc_index >= 0; ++i) {
        struct drm_mode_get_plane p = {.plane_id = plane_ids[i]};
        uint64_t type = 0;
        if (io(d->fd, DRM_IOCTL_MODE_GETPLANE, &p) || !(p.possible_crtcs & (1u << crtc_index))) continue;
        if (property(d->fd, plane_ids[i], DRM_MODE_OBJECT_PLANE, "type", &type) && type == PRIMARY)
            d->plane = plane_ids[i];
    }
    if (!d->crtc || !d->plane) FAIL("no CRTC and plane for the connector");
    const char *src[4] = {"SRC_X", "SRC_Y", "SRC_W", "SRC_H"}, *dst[4] = {"CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H"};
    d->connector_crtc = property(d->fd, d->connector, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL);
    d->crtc_mode = property(d->fd, d->crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID", NULL);
    d->crtc_active = property(d->fd, d->crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);
    d->plane_fb = property(d->fd, d->plane, DRM_MODE_OBJECT_PLANE, "FB_ID", NULL);
    d->plane_crtc = property(d->fd, d->plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID", NULL);
    for (int i = 0; i < 4; ++i) {
        d->plane_src[i] = property(d->fd, d->plane, DRM_MODE_OBJECT_PLANE, src[i], NULL);
        d->plane_dst[i] = property(d->fd, d->plane, DRM_MODE_OBJECT_PLANE, dst[i], NULL);
    }
    if (writeback) {
        d->writeback_fb = property(d->fd, d->connector, DRM_MODE_OBJECT_CONNECTOR, "WRITEBACK_FB_ID", NULL);
        d->writeback_fence = property(d->fd, d->connector, DRM_MODE_OBJECT_CONNECTOR, "WRITEBACK_OUT_FENCE_PTR", NULL);
        if (!d->writeback_fb || !d->writeback_fence) FAIL("the writeback connector lacks its properties");
    }
    if (!d->connector_crtc || !d->crtc_mode || !d->crtc_active || !d->plane_fb || !d->plane_crtc) FAIL("missing properties");
    struct drm_mode_create_blob blob = {.data = (uintptr_t)&d->mode, .length = sizeof(d->mode)};
    if (io(d->fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob)) FAIL("cannot make the mode");
    d->mode_blob = blob.blob_id;
    for (int i = 0; i < 2; ++i)
        if (!make_buffer(d->fd, d->mode.hdisplay, d->mode.vdisplay, &d->buffers[i])) FAIL("cannot make buffers");
    if (writeback && !make_buffer(d->fd, d->mode.hdisplay, d->mode.vdisplay, &d->capture)) FAIL("cannot make buffers");
    if (!(d->shadow = malloc((size_t)d->mode.hdisplay * d->mode.vdisplay * sizeof(*d->shadow)))) FAIL("out of memory");
    return d;
}

uint32_t *vega_display_buffer(VegaDisplay *d, unsigned *width, unsigned *height, size_t *stride) {
    *width = d->mode.hdisplay;
    *height = d->mode.vdisplay;
    *stride = d->mode.hdisplay;
    return d->shadow;
}

const uint32_t *vega_display_captured(const VegaDisplay *d, size_t *stride) {
    *stride = d->capture.pitch / 4;
    return d->writeback ? d->capture.pixels : NULL;
}

/* One atomic commit: the back buffer on the plane (and, the first time, the
 * mode on the CRTC and the CRTC on the connector); on writeback, the capture
 * buffer and a fence for when it holds the frame. */
bool vega_display_show(VegaDisplay *d) {
    uint32_t objects[3], counts[3] = {0}, ids[24];
    uint64_t values[24];
    unsigned n = 0, o = 0;
    int fence = -1;
#define SET(object, id, value) do { \
        if (!o || objects[o - 1] != (object)) objects[o++] = (object); \
        ++counts[o - 1]; ids[n] = (id); values[n++] = (value); } while (0)
    Buffer *shown = &d->buffers[d->back];
    for (unsigned y = 0; y < d->mode.vdisplay; ++y)
        memcpy((unsigned char *)shown->pixels + (size_t)y * shown->pitch, d->shadow + (size_t)y * d->mode.hdisplay,
               (size_t)d->mode.hdisplay * sizeof(*d->shadow));
    if (!d->set || d->writeback) SET(d->connector, d->connector_crtc, d->crtc);
    if (d->writeback) {
        SET(d->connector, d->writeback_fb, d->capture.id);
        SET(d->connector, d->writeback_fence, (uintptr_t)&fence);
    }
    if (!d->set) {
        SET(d->crtc, d->crtc_mode, d->mode_blob);
        SET(d->crtc, d->crtc_active, 1);
    }
    SET(d->plane, d->plane_fb, shown->id);
    SET(d->plane, d->plane_crtc, d->crtc);
    uint64_t size[4] = {0, 0, (uint64_t)d->mode.hdisplay << 16, (uint64_t)d->mode.vdisplay << 16};
    uint64_t place[4] = {0, 0, d->mode.hdisplay, d->mode.vdisplay};
    for (int i = 0; i < 4; ++i) {
        if (d->plane_src[i]) SET(d->plane, d->plane_src[i], size[i]);
        if (d->plane_dst[i]) SET(d->plane, d->plane_dst[i], place[i]);
    }
#undef SET
    struct drm_mode_atomic commit = {.flags = d->set ? 0 : DRM_MODE_ATOMIC_ALLOW_MODESET, .count_objs = o,
                                     .objs_ptr = (uintptr_t)objects, .count_props_ptr = (uintptr_t)counts,
                                     .props_ptr = (uintptr_t)ids, .prop_values_ptr = (uintptr_t)values};
    if (io(d->fd, DRM_IOCTL_MODE_ATOMIC, &commit)) return false;
    d->set = true;
    d->back ^= 1;
    if (fence >= 0) { /* the frame is in the capture buffer when the fence signals */
        struct pollfd wait = {.fd = fence, .events = POLLIN};
        bool done = poll(&wait, 1, 1000) == 1;
        close(fence);
        if (!done) return false;
    }
    return true;
}

void vega_display_close(VegaDisplay *d) {
    if (!d) return;
    if (d->fd >= 0) {
        for (int i = 0; i < 2; ++i) free_buffer(d->fd, &d->buffers[i]);
        free_buffer(d->fd, &d->capture);
        free(d->shadow);
        if (d->mode_blob) {
            struct drm_mode_destroy_blob blob = {.blob_id = d->mode_blob};
            io(d->fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &blob);
        }
        close(d->fd);
    }
    free(d);
}
