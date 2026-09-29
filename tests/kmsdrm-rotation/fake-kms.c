/*
 * tsp-mc9m.41.924.16.12 hermetic test support: a fake libdrm + libgbm +
 * libEGL + libGLESv2 in ONE shared object. run.sh installs it as
 * libdrm.so.2, libgbm.so.1, libEGL.so.1 and libGLESv2.so.2 (symlinks to the
 * same file, so every soname resolves to the same loaded object and shares
 * this state) and SDL's real KMSDRM backend dlopens it.
 *
 * It models one 720x1280 DSI panel (connector 31, encoder 30, CRTC 40,
 * primary plane 50, fbcon framebuffer 7) and enforces the contracts the real
 * stack enforces: kernel property/object typing, the "panel orientation" enum
 * table, Mesa's GBM surface buffer locking, EGL current-surface rules, and
 * scanout framebuffers that must match the mode. Violations are counted in
 * FakeKmsReport.errors and printed; the test fails on any.
 *
 * Environment:
 *   FAKE_KMS_PANEL_ORIENTATION  Normal | Left Side Up | Right Side Up |
 *                               Upside Down | none      (default Normal)
 *   FAKE_KMS_ATOMIC             1 | 0   atomic modesetting   (default 1)
 *   FAKE_EGL_NATIVE_FENCE       1 | 0   EGL_ANDROID_native_fence_sync (1)
 *   FAKE_EGL_FENCE_SYNC         1 | 0   EGL_KHR_fence_sync + wait_sync (1)
 *   FAKE_EGL_FAIL_MAKECURRENT_SURFACE  N: the first eglMakeCurrent that binds
 *                               a context to an EGL surface made from gbm
 *                               surface N (in creation order) fails
 *   FAKE_KMS_FLIP_MS            N > 0: an atomic flip completes N ms after its
 *                               commit (default 0: at once). Its OUT_FENCE is a
 *                               timerfd that polls readable then, as the
 *                               kernel's sync_file does, and a NONBLOCK commit
 *                               while a flip is pending fails with -EBUSY, as
 *                               the atomic helpers' stall check does (counted
 *                               as a contract violation). eglClientWaitSyncKHR
 *                               on a fence imported from that fd still returns
 *                               at once: that is what Mesa's Zink does.
 *
 * It is also installed as libvulkan.so.1: vkGetInstanceProcAddr and
 * vkEnumerateInstanceExtensionProperties (VK_KHR_surface, VK_KHR_display) are
 * enough for SDL to load Vulkan and create a Vulkan window; no instance or
 * surface is modelled.
 *
 * Built with -DFAKE_KMS_OMIT_GL_LINK_PROGRAM, the stack lacks glLinkProgram (an
 * entry point in the middle of the rotate pass's table): it is neither exported
 * (dlsym fails) nor returned by eglGetProcAddress.
 */
#define _GNU_SOURCE
#define EGL_EGLEXT_PROTOTYPES 1
#define GL_GLEXT_PROTOTYPES 1

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "fake-kms.h"

#define FAKE_EXPORT __attribute__((visibility("default")))

enum {
    OBJ_ENCODER = 30,
    OBJ_CONNECTOR = 31,
    OBJ_CRTC = 40,
    OBJ_PLANE = 50,
    FB_FBCON = 7,
};

enum {
    PROP_TYPE = 100,
    PROP_FB_ID,
    PROP_PLANE_CRTC_ID,
    PROP_SRC_X,
    PROP_SRC_Y,
    PROP_SRC_W,
    PROP_SRC_H,
    PROP_CRTC_X,
    PROP_CRTC_Y,
    PROP_CRTC_W,
    PROP_CRTC_H,
    PROP_IN_FENCE_FD,
    PROP_ACTIVE = 120,
    PROP_MODE_ID,
    PROP_OUT_FENCE_PTR,
    PROP_CONN_CRTC_ID = 130,
    PROP_PANEL_ORIENTATION,
};

typedef struct FakeProp
{
    uint32_t id;
    uint32_t obj;
    uint32_t obj_type;
    const char *name;
    uint32_t flags;
} FakeProp;

static const FakeProp g_props[] = {
    { PROP_TYPE, OBJ_PLANE, DRM_MODE_OBJECT_PLANE, "type", DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE },
    { PROP_FB_ID, OBJ_PLANE, DRM_MODE_OBJECT_PLANE, "FB_ID", DRM_MODE_PROP_OBJECT },
    { PROP_PLANE_CRTC_ID, OBJ_PLANE, DRM_MODE_OBJECT_PLANE, "CRTC_ID", DRM_MODE_PROP_OBJECT },
    { PROP_SRC_X, OBJ_PLANE, DRM_MODE_OBJECT_PLANE, "SRC_X", DRM_MODE_PROP_RANGE },
    { PROP_SRC_Y, OBJ_PLANE, DRM_MODE_OBJECT_PLANE, "SRC_Y", DRM_MODE_PROP_RANGE },
    { PROP_SRC_W, OBJ_PLANE, DRM_MODE_OBJECT_PLANE, "SRC_W", DRM_MODE_PROP_RANGE },
    { PROP_SRC_H, OBJ_PLANE, DRM_MODE_OBJECT_PLANE, "SRC_H", DRM_MODE_PROP_RANGE },
    { PROP_CRTC_X, OBJ_PLANE, DRM_MODE_OBJECT_PLANE, "CRTC_X", DRM_MODE_PROP_SIGNED_RANGE },
    { PROP_CRTC_Y, OBJ_PLANE, DRM_MODE_OBJECT_PLANE, "CRTC_Y", DRM_MODE_PROP_SIGNED_RANGE },
    { PROP_CRTC_W, OBJ_PLANE, DRM_MODE_OBJECT_PLANE, "CRTC_W", DRM_MODE_PROP_RANGE },
    { PROP_CRTC_H, OBJ_PLANE, DRM_MODE_OBJECT_PLANE, "CRTC_H", DRM_MODE_PROP_RANGE },
    { PROP_IN_FENCE_FD, OBJ_PLANE, DRM_MODE_OBJECT_PLANE, "IN_FENCE_FD", DRM_MODE_PROP_SIGNED_RANGE },
    { PROP_ACTIVE, OBJ_CRTC, DRM_MODE_OBJECT_CRTC, "ACTIVE", DRM_MODE_PROP_RANGE },
    { PROP_MODE_ID, OBJ_CRTC, DRM_MODE_OBJECT_CRTC, "MODE_ID", DRM_MODE_PROP_BLOB },
    { PROP_OUT_FENCE_PTR, OBJ_CRTC, DRM_MODE_OBJECT_CRTC, "OUT_FENCE_PTR", DRM_MODE_PROP_RANGE },
    { PROP_CONN_CRTC_ID, OBJ_CONNECTOR, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", DRM_MODE_PROP_OBJECT },
    { PROP_PANEL_ORIENTATION, OBJ_CONNECTOR, DRM_MODE_OBJECT_CONNECTOR, "panel orientation", DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE },
};

/* The kernel's table and enum values: drm_connector.c drm_panel_orientation_enum_list
   (DRM_MODE_PANEL_ORIENTATION_NORMAL = 0 .. RIGHT_UP = 3). */
static const char *const g_orientation_names[] = { "Normal", "Upside Down", "Left Side Up", "Right Side Up" };
static const char *const g_plane_type_names[] = { "Overlay", "Primary", "Cursor" };

static FakeKmsReport g_report;
static int g_initialized;
static int g_orientation_value = 0;  // index into g_orientation_names
static int g_has_orientation = 1;
static int g_atomic = 1;
static int g_native_fence = 1;
static int g_fence_sync = 1;
static int g_fail_makecurrent_surface = -1;
static int g_flip_ms = 0;
static uint64_t g_flip_done_ns;  // CLOCK_MONOTONIC time the last flip completes
static char g_display_extensions[512];
static drmModeModeInfo g_mode;

static void fake_error(const char *fmt, ...)
{
    char message[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    if (g_report.errors++ == 0) {
        snprintf(g_report.first_error, sizeof(g_report.first_error), "%s", message);
    }
    fprintf(stderr, "FAKE-KMS ERROR: %s\n", message);
}

static int env_flag(const char *name, int fallback)
{
    const char *value = getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    return strcmp(value, "0") != 0;
}

static void fake_init(void)
{
    const char *orientation;
    size_t i;

    if (g_initialized) {
        return;
    }
    g_initialized = 1;

    orientation = getenv("FAKE_KMS_PANEL_ORIENTATION");
    if (!orientation || !*orientation) {
        orientation = "Normal";
    }
    g_has_orientation = strcmp(orientation, "none") != 0;
    if (g_has_orientation) {
        g_orientation_value = -1;
        for (i = 0; i < sizeof(g_orientation_names) / sizeof(g_orientation_names[0]); ++i) {
            if (strcmp(orientation, g_orientation_names[i]) == 0) {
                g_orientation_value = (int)i;
            }
        }
        if (g_orientation_value < 0) {
            fake_error("unknown FAKE_KMS_PANEL_ORIENTATION '%s'", orientation);
            g_orientation_value = 0;
        }
    }
    g_atomic = env_flag("FAKE_KMS_ATOMIC", 1);
    g_native_fence = env_flag("FAKE_EGL_NATIVE_FENCE", 1);
    g_fence_sync = env_flag("FAKE_EGL_FENCE_SYNC", 1);
    if (getenv("FAKE_EGL_FAIL_MAKECURRENT_SURFACE") && *getenv("FAKE_EGL_FAIL_MAKECURRENT_SURFACE")) {
        g_fail_makecurrent_surface = atoi(getenv("FAKE_EGL_FAIL_MAKECURRENT_SURFACE"));
    }
    if (getenv("FAKE_KMS_FLIP_MS") && *getenv("FAKE_KMS_FLIP_MS")) {
        g_flip_ms = atoi(getenv("FAKE_KMS_FLIP_MS"));
    }

    snprintf(g_display_extensions, sizeof(g_display_extensions),
             "EGL_KHR_image_base EGL_KHR_image_pixmap EGL_KHR_surfaceless_context "
             "EGL_KHR_no_config_context EGL_KHR_create_context%s%s",
             g_fence_sync ? " EGL_KHR_fence_sync EGL_KHR_wait_sync" : "",
             g_native_fence ? " EGL_ANDROID_native_fence_sync" : "");

    memset(&g_mode, 0, sizeof(g_mode));
    g_mode.clock = 72000;
    g_mode.hdisplay = FAKE_KMS_PANEL_W;
    g_mode.hsync_start = FAKE_KMS_PANEL_W + 20;
    g_mode.hsync_end = FAKE_KMS_PANEL_W + 30;
    g_mode.htotal = 800;
    g_mode.vdisplay = FAKE_KMS_PANEL_H;
    g_mode.vsync_start = FAKE_KMS_PANEL_H + 120;
    g_mode.vsync_end = FAKE_KMS_PANEL_H + 130;
    g_mode.vtotal = 1500;
    g_mode.vrefresh = 60;
    g_mode.type = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
    snprintf(g_mode.name, sizeof(g_mode.name), "%dx%d", FAKE_KMS_PANEL_W, FAKE_KMS_PANEL_H);

    g_report.scanout_surface = -1;
    g_report.current_draw_surface = -1;
}

FAKE_EXPORT const FakeKmsReport *fake_kms_get_report(void)
{
    fake_init();
    return &g_report;
}

/* ------------------------------------------------------------------ GBM -- */

struct gbm_device
{
    int fd;
};

struct gbm_bo
{
    struct gbm_device *dev;
    int surface;          // index into g_surfaces, -1 for a standalone bo
    uint32_t w, h, format, handle;
    int locked;
    int frame;
    int destroyed;
    void *user_data;
    void (*destroy_user_data)(struct gbm_bo *, void *);
};

struct gbm_surface
{
    int index;
    struct gbm_device *dev;
    uint32_t w, h, format, flags;
    struct gbm_bo bos[FAKE_KMS_BOS_PER_SURFACE];
    int back;
    int front_pending;
    int alive;
};

static struct gbm_surface *g_surfaces[FAKE_KMS_MAX_SURFACES];
static uint32_t g_next_handle = 1;

static void report_surface(const struct gbm_surface *gs)
{
    FakeKmsSurfaceReport *r = &g_report.surfaces[gs->index];
    int i, locked = 0;

    for (i = 0; i < FAKE_KMS_BOS_PER_SURFACE; ++i) {
        locked += gs->bos[i].locked;
    }
    r->w = (int)gs->w;
    r->h = (int)gs->h;
    r->flags = gs->flags;
    r->alive = gs->alive;
    r->locked_now = locked;
    if (locked > r->locked_max) {
        r->locked_max = locked;
    }
}

static void destroy_bo_user_data(struct gbm_bo *bo)
{
    if (bo->destroy_user_data) {
        bo->destroy_user_data(bo, bo->user_data);
    }
    bo->destroy_user_data = NULL;
    bo->user_data = NULL;
    bo->destroyed = 1;
}

FAKE_EXPORT struct gbm_device *gbm_create_device(int fd)
{
    struct gbm_device *dev = calloc(1, sizeof(*dev));
    fake_init();
    dev->fd = fd;
    return dev;
}

FAKE_EXPORT void gbm_device_destroy(struct gbm_device *gbm)
{
    free(gbm);
}

FAKE_EXPORT int gbm_device_is_format_supported(struct gbm_device *gbm, uint32_t format, uint32_t flags)
{
    (void)gbm;
    // No cursor support: SDL then skips its cursor buffer entirely.
    return format == GBM_FORMAT_ARGB8888 && !(flags & GBM_BO_USE_CURSOR);
}

FAKE_EXPORT struct gbm_bo *gbm_bo_create(struct gbm_device *gbm, uint32_t width, uint32_t height,
                                         uint32_t format, uint32_t flags)
{
    struct gbm_bo *bo = calloc(1, sizeof(*bo));
    (void)flags;
    bo->dev = gbm;
    bo->surface = -1;
    bo->w = width;
    bo->h = height;
    bo->format = format;
    bo->handle = g_next_handle++;
    return bo;
}

FAKE_EXPORT void gbm_bo_destroy(struct gbm_bo *bo)
{
    if (bo && bo->surface < 0) {
        destroy_bo_user_data(bo);
        free(bo);
    } else if (bo) {
        fake_error("gbm_bo_destroy on a surface buffer");
    }
}

FAKE_EXPORT uint32_t gbm_bo_get_width(struct gbm_bo *bo) { return bo->w; }
FAKE_EXPORT uint32_t gbm_bo_get_height(struct gbm_bo *bo) { return bo->h; }
FAKE_EXPORT uint32_t gbm_bo_get_stride(struct gbm_bo *bo) { return bo->w * 4; }
FAKE_EXPORT uint32_t gbm_bo_get_format(struct gbm_bo *bo) { return bo->format; }
FAKE_EXPORT struct gbm_device *gbm_bo_get_device(struct gbm_bo *bo) { return bo->dev; }
FAKE_EXPORT uint64_t gbm_bo_get_modifier(struct gbm_bo *bo) { (void)bo; return 0; /* DRM_FORMAT_MOD_LINEAR */ }
FAKE_EXPORT int gbm_bo_get_plane_count(struct gbm_bo *bo) { (void)bo; return 1; }
FAKE_EXPORT uint32_t gbm_bo_get_offset(struct gbm_bo *bo, int plane) { (void)bo; (void)plane; return 0; }
FAKE_EXPORT uint32_t gbm_bo_get_stride_for_plane(struct gbm_bo *bo, int plane) { (void)plane; return bo->w * 4; }

FAKE_EXPORT union gbm_bo_handle gbm_bo_get_handle(struct gbm_bo *bo)
{
    union gbm_bo_handle handle;
    handle.u64 = 0;
    handle.u32 = bo->handle;
    return handle;
}

FAKE_EXPORT union gbm_bo_handle gbm_bo_get_handle_for_plane(struct gbm_bo *bo, int plane)
{
    (void)plane;
    return gbm_bo_get_handle(bo);
}

FAKE_EXPORT int gbm_bo_write(struct gbm_bo *bo, const void *buf, size_t count)
{
    (void)bo;
    (void)buf;
    (void)count;
    return 0;
}

FAKE_EXPORT void gbm_bo_set_user_data(struct gbm_bo *bo, void *data, void (*destroy_user_data)(struct gbm_bo *, void *))
{
    bo->user_data = data;
    bo->destroy_user_data = destroy_user_data;
}

FAKE_EXPORT void *gbm_bo_get_user_data(struct gbm_bo *bo)
{
    return bo->user_data;
}

FAKE_EXPORT struct gbm_surface *gbm_surface_create(struct gbm_device *gbm, uint32_t width, uint32_t height,
                                                   uint32_t format, uint32_t flags)
{
    struct gbm_surface *gs;
    int i;

    fake_init();
    if (g_report.surfaces_created >= FAKE_KMS_MAX_SURFACES) {
        fake_error("too many gbm surfaces");
        errno = ENOMEM;
        return NULL;
    }
    gs = calloc(1, sizeof(*gs));
    gs->index = g_report.surfaces_created++;
    gs->dev = gbm;
    gs->w = width;
    gs->h = height;
    gs->format = format;
    gs->flags = flags;
    gs->back = 0;
    gs->front_pending = -1;
    gs->alive = 1;
    for (i = 0; i < FAKE_KMS_BOS_PER_SURFACE; ++i) {
        gs->bos[i].dev = gbm;
        gs->bos[i].surface = gs->index;
        gs->bos[i].w = width;
        gs->bos[i].h = height;
        gs->bos[i].format = format;
        gs->bos[i].handle = g_next_handle++;
    }
    g_surfaces[gs->index] = gs;
    report_surface(gs);
    return gs;
}

FAKE_EXPORT void gbm_surface_destroy(struct gbm_surface *gs)
{
    int i;

    if (!gs || !gs->alive) {
        fake_error("gbm_surface_destroy on a dead surface");
        return;
    }
    for (i = 0; i < FAKE_KMS_BOS_PER_SURFACE; ++i) {
        if (gs->bos[i].locked) {
            fake_error("gbm_surface_destroy: surface %d (%ux%u) still has buffer %d locked",
                       gs->index, gs->w, gs->h, i);
        }
        destroy_bo_user_data(&gs->bos[i]);
        gs->bos[i].locked = 0;
    }
    gs->alive = 0;
    report_surface(gs);
    // The struct is kept (not freed) so stale pointers stay diagnosable.
}

FAKE_EXPORT struct gbm_bo *gbm_surface_lock_front_buffer(struct gbm_surface *gs)
{
    struct gbm_bo *bo;

    if (!gs || !gs->alive) {
        fake_error("gbm_surface_lock_front_buffer on a dead surface");
        return NULL;
    }
    if (gs->front_pending < 0) {
        fake_error("gbm_surface_lock_front_buffer on surface %d (%ux%u) with no swapped buffer",
                   gs->index, gs->w, gs->h);
        return NULL;
    }
    bo = &gs->bos[gs->front_pending];
    bo->locked = 1;
    gs->front_pending = -1;
    report_surface(gs);
    return bo;
}

FAKE_EXPORT void gbm_surface_release_buffer(struct gbm_surface *gs, struct gbm_bo *bo)
{
    if (!gs || !bo) {
        fake_error("gbm_surface_release_buffer with a NULL surface or buffer");
        return;
    }
    if (bo->surface != gs->index) {
        fake_error("gbm_surface_release_buffer: buffer of surface %d released to surface %d",
                   bo->surface, gs->index);
        return;
    }
    if (!bo->locked) {
        fake_error("gbm_surface_release_buffer: buffer of surface %d was not locked", gs->index);
        return;
    }
    bo->locked = 0;
    report_surface(gs);
}

static struct gbm_bo *bo_for_handle(uint32_t handle)
{
    int s, i;

    for (s = 0; s < g_report.surfaces_created; ++s) {
        for (i = 0; i < FAKE_KMS_BOS_PER_SURFACE; ++i) {
            if (g_surfaces[s]->bos[i].handle == handle && !g_surfaces[s]->bos[i].destroyed) {
                return &g_surfaces[s]->bos[i];
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ DRM -- */

typedef struct FakeFb
{
    uint32_t id;
    int w, h;
    int surface;
    int alive;
} FakeFb;

static FakeFb g_fbs[512];
static int g_num_fbs;
static uint32_t g_next_fb = 1000;
static uint32_t g_next_blob = 900;
static void *g_pending_flip_data;
static int g_pending_flip;

static FakeFb *find_fb(uint32_t id)
{
    int i;

    if (id == FB_FBCON) {
        static FakeFb fbcon = { FB_FBCON, FAKE_KMS_PANEL_W, FAKE_KMS_PANEL_H, -1, 1 };
        return &fbcon;
    }
    for (i = 0; i < g_num_fbs; ++i) {
        if (g_fbs[i].id == id) {
            return &g_fbs[i];
        }
    }
    return NULL;
}

static int add_fb(uint32_t width, uint32_t height, uint32_t handle, uint32_t *buf_id)
{
    struct gbm_bo *bo = bo_for_handle(handle);
    FakeFb *fb;

    if (g_num_fbs >= (int)(sizeof(g_fbs) / sizeof(g_fbs[0]))) {
        fake_error("too many framebuffers");
        return -ENOMEM;
    }
    fb = &g_fbs[g_num_fbs++];
    fb->id = g_next_fb++;
    fb->w = (int)width;
    fb->h = (int)height;
    fb->surface = bo ? bo->surface : -1;
    fb->alive = 1;
    if (bo && (bo->w != width || bo->h != height)) {
        fake_error("AddFB %ux%u for a %ux%u buffer", width, height, bo->w, bo->h);
    }
    *buf_id = fb->id;
    return 0;
}

static int scanout(uint32_t fb_id, const char *what)
{
    FakeFb *fb = find_fb(fb_id);

    if (!fb || !fb->alive) {
        fake_error("%s: framebuffer %u does not exist", what, fb_id);
        return -ENOENT;
    }
    if (fb->w != g_mode.hdisplay || fb->h != g_mode.vdisplay) {
        fake_error("%s: framebuffer %u is %dx%d, the %ux%u mode needs a panel-native buffer",
                   what, fb_id, fb->w, fb->h, g_mode.hdisplay, g_mode.vdisplay);
        return -EINVAL;
    }
    g_report.scanouts++;
    g_report.scanout_surface = fb->surface;
    g_report.scanout_w = fb->w;
    g_report.scanout_h = fb->h;
    return 0;
}

FAKE_EXPORT void drmModeFreeResources(drmModeResPtr ptr)
{
    if (ptr) {
        free(ptr->fbs);
        free(ptr->crtcs);
        free(ptr->connectors);
        free(ptr->encoders);
        free(ptr);
    }
}

FAKE_EXPORT void drmModeFreeFB(drmModeFBPtr ptr) { free(ptr); }
FAKE_EXPORT void drmModeFreeCrtc(drmModeCrtcPtr ptr) { free(ptr); }
FAKE_EXPORT void drmModeFreeEncoder(drmModeEncoderPtr ptr) { free(ptr); }

FAKE_EXPORT void drmModeFreeConnector(drmModeConnectorPtr ptr)
{
    if (ptr) {
        free(ptr->modes);
        free(ptr->props);
        free(ptr->prop_values);
        free(ptr->encoders);
        free(ptr);
    }
}

FAKE_EXPORT int drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
    (void)fd;
    *value = 0;
    if (capability == DRM_CAP_CURSOR_WIDTH || capability == DRM_CAP_CURSOR_HEIGHT) {
        *value = 64;
    }
    return 0;
}

FAKE_EXPORT int drmSetMaster(int fd) { (void)fd; return 0; }
FAKE_EXPORT int drmDropMaster(int fd) { (void)fd; return 0; }
FAKE_EXPORT int drmAuthMagic(int fd, drm_magic_t magic) { (void)fd; (void)magic; return 0; }

FAKE_EXPORT int drmSetClientCap(int fd, uint64_t capability, uint64_t value)
{
    (void)fd;
    (void)value;
    fake_init();
    if (capability == DRM_CLIENT_CAP_ATOMIC && !g_atomic) {
        errno = EOPNOTSUPP;
        return -EOPNOTSUPP;
    }
    return 0;
}

FAKE_EXPORT drmModeResPtr drmModeGetResources(int fd)
{
    drmModeResPtr res = calloc(1, sizeof(*res));
    (void)fd;
    fake_init();
    res->count_crtcs = 1;
    res->crtcs = calloc(1, sizeof(uint32_t));
    res->crtcs[0] = OBJ_CRTC;
    res->count_connectors = 1;
    res->connectors = calloc(1, sizeof(uint32_t));
    res->connectors[0] = OBJ_CONNECTOR;
    res->count_encoders = 1;
    res->encoders = calloc(1, sizeof(uint32_t));
    res->encoders[0] = OBJ_ENCODER;
    res->min_width = 1;
    res->max_width = 4096;
    res->min_height = 1;
    res->max_height = 4096;
    return res;
}

FAKE_EXPORT drmModeConnectorPtr drmModeGetConnector(int fd, uint32_t connectorId)
{
    drmModeConnectorPtr conn;
    (void)fd;
    fake_init();
    if (connectorId != OBJ_CONNECTOR) {
        errno = ENOENT;
        return NULL;
    }
    conn = calloc(1, sizeof(*conn));
    conn->connector_id = OBJ_CONNECTOR;
    conn->encoder_id = OBJ_ENCODER;
    conn->connector_type = DRM_MODE_CONNECTOR_DSI;
    conn->connector_type_id = 1;
    conn->connection = DRM_MODE_CONNECTED;
    conn->mmWidth = 62;
    conn->mmHeight = 110;
    conn->subpixel = DRM_MODE_SUBPIXEL_UNKNOWN;
    conn->count_modes = 1;
    conn->modes = calloc(1, sizeof(drmModeModeInfo));
    conn->modes[0] = g_mode;
    conn->count_encoders = 1;
    conn->encoders = calloc(1, sizeof(uint32_t));
    conn->encoders[0] = OBJ_ENCODER;
    return conn;
}

FAKE_EXPORT const char *drmModeGetConnectorTypeName(uint32_t connector_type)
{
    return connector_type == DRM_MODE_CONNECTOR_DSI ? "DSI" : NULL;
}

FAKE_EXPORT drmModeEncoderPtr drmModeGetEncoder(int fd, uint32_t encoder_id)
{
    drmModeEncoderPtr enc;
    (void)fd;
    if (encoder_id != OBJ_ENCODER) {
        errno = ENOENT;
        return NULL;
    }
    enc = calloc(1, sizeof(*enc));
    enc->encoder_id = OBJ_ENCODER;
    enc->encoder_type = DRM_MODE_ENCODER_DSI;
    enc->crtc_id = OBJ_CRTC;
    enc->possible_crtcs = 1;
    return enc;
}

FAKE_EXPORT drmModeCrtcPtr drmModeGetCrtc(int fd, uint32_t crtcId)
{
    drmModeCrtcPtr crtc;
    (void)fd;
    fake_init();
    if (crtcId != OBJ_CRTC) {
        errno = ENOENT;
        return NULL;
    }
    crtc = calloc(1, sizeof(*crtc));
    crtc->crtc_id = OBJ_CRTC;
    crtc->buffer_id = FB_FBCON;
    crtc->width = FAKE_KMS_PANEL_W;
    crtc->height = FAKE_KMS_PANEL_H;
    crtc->mode_valid = 1;
    crtc->mode = g_mode;
    return crtc;
}

FAKE_EXPORT drmModePlaneResPtr drmModeGetPlaneResources(int fd)
{
    drmModePlaneResPtr res = calloc(1, sizeof(*res));
    (void)fd;
    res->count_planes = 1;
    res->planes = calloc(1, sizeof(uint32_t));
    res->planes[0] = OBJ_PLANE;
    return res;
}

FAKE_EXPORT drmModePlanePtr drmModeGetPlane(int fd, uint32_t plane_id)
{
    drmModePlanePtr plane;
    (void)fd;
    if (plane_id != OBJ_PLANE) {
        errno = ENOENT;
        return NULL;
    }
    plane = calloc(1, sizeof(*plane));
    plane->plane_id = OBJ_PLANE;
    plane->crtc_id = OBJ_CRTC;
    plane->fb_id = FB_FBCON;
    plane->possible_crtcs = 1;
    return plane;
}

FAKE_EXPORT void drmModeFreePlane(drmModePlanePtr ptr)
{
    if (ptr) {
        free(ptr->formats);
        free(ptr);
    }
}

FAKE_EXPORT void drmModeFreePlaneResources(drmModePlaneResPtr ptr)
{
    if (ptr) {
        free(ptr->planes);
        free(ptr);
    }
}

static const FakeProp *find_prop(uint32_t id)
{
    size_t i;
    for (i = 0; i < sizeof(g_props) / sizeof(g_props[0]); ++i) {
        if (g_props[i].id == id) {
            return &g_props[i];
        }
    }
    return NULL;
}

// Objects are typed, as in the kernel: an id looked up as the wrong type does not exist.
FAKE_EXPORT drmModeObjectPropertiesPtr drmModeObjectGetProperties(int fd, uint32_t object_id, uint32_t object_type)
{
    drmModeObjectPropertiesPtr props;
    size_t i;
    int known = 0;
    (void)fd;

    fake_init();
    known = (object_id == OBJ_CONNECTOR && object_type == DRM_MODE_OBJECT_CONNECTOR) ||
            (object_id == OBJ_CRTC && object_type == DRM_MODE_OBJECT_CRTC) ||
            (object_id == OBJ_PLANE && object_type == DRM_MODE_OBJECT_PLANE);
    if (!known) {
        errno = ENOENT;
        return NULL;
    }

    props = calloc(1, sizeof(*props));
    props->props = calloc(sizeof(g_props) / sizeof(g_props[0]), sizeof(uint32_t));
    props->prop_values = calloc(sizeof(g_props) / sizeof(g_props[0]), sizeof(uint64_t));
    for (i = 0; i < sizeof(g_props) / sizeof(g_props[0]); ++i) {
        if (g_props[i].obj != object_id || g_props[i].obj_type != object_type) {
            continue;
        }
        if (g_props[i].id == PROP_PANEL_ORIENTATION && !g_has_orientation) {
            continue;
        }
        props->props[props->count_props] = g_props[i].id;
        if (g_props[i].id == PROP_TYPE) {
            props->prop_values[props->count_props] = DRM_PLANE_TYPE_PRIMARY;
        } else if (g_props[i].id == PROP_PANEL_ORIENTATION) {
            props->prop_values[props->count_props] = (uint64_t)g_orientation_value;
        } else if (g_props[i].id == PROP_FB_ID) {
            props->prop_values[props->count_props] = FB_FBCON;
        }
        props->count_props++;
    }
    return props;
}

FAKE_EXPORT void drmModeFreeObjectProperties(drmModeObjectPropertiesPtr ptr)
{
    if (ptr) {
        free(ptr->props);
        free(ptr->prop_values);
        free(ptr);
    }
}

FAKE_EXPORT drmModePropertyPtr drmModeGetProperty(int fd, uint32_t propertyId)
{
    const FakeProp *fp = find_prop(propertyId);
    drmModePropertyPtr prop;
    const char *const *names = NULL;
    int count = 0, i;
    (void)fd;

    fake_init();
    if (!fp) {
        errno = ENOENT;
        return NULL;
    }
    prop = calloc(1, sizeof(*prop));
    prop->prop_id = fp->id;
    prop->flags = fp->flags;
    snprintf(prop->name, sizeof(prop->name), "%s", fp->name);
    if (fp->id == PROP_PANEL_ORIENTATION) {
        names = g_orientation_names;
        count = 4;
    } else if (fp->id == PROP_TYPE) {
        names = g_plane_type_names;
        count = 3;
    }
    if (names) {
        prop->count_enums = count;
        prop->enums = calloc((size_t)count, sizeof(*prop->enums));
        for (i = 0; i < count; ++i) {
            prop->enums[i].value = (uint64_t)i;
            snprintf(prop->enums[i].name, sizeof(prop->enums[i].name), "%s", names[i]);
        }
    }
    return prop;
}

FAKE_EXPORT void drmModeFreeProperty(drmModePropertyPtr ptr)
{
    if (ptr) {
        free(ptr->values);
        free(ptr->enums);
        free(ptr->blob_ids);
        free(ptr);
    }
}

FAKE_EXPORT int drmModeAddFB(int fd, uint32_t width, uint32_t height, uint8_t depth,
                             uint8_t bpp, uint32_t pitch, uint32_t bo_handle, uint32_t *buf_id)
{
    (void)fd;
    (void)depth;
    (void)bpp;
    (void)pitch;
    return add_fb(width, height, bo_handle, buf_id);
}

FAKE_EXPORT int drmModeAddFB2(int fd, uint32_t width, uint32_t height, uint32_t pixel_format,
                              const uint32_t bo_handles[4], const uint32_t pitches[4],
                              const uint32_t offsets[4], uint32_t *buf_id, uint32_t flags)
{
    (void)fd;
    (void)pixel_format;
    (void)pitches;
    (void)offsets;
    (void)flags;
    return add_fb(width, height, bo_handles[0], buf_id);
}

FAKE_EXPORT int drmModeAddFB2WithModifiers(int fd, uint32_t width, uint32_t height, uint32_t pixel_format,
                                           const uint32_t bo_handles[4], const uint32_t pitches[4],
                                           const uint32_t offsets[4], const uint64_t modifier[4],
                                           uint32_t *buf_id, uint32_t flags)
{
    (void)modifier;
    return drmModeAddFB2(fd, width, height, pixel_format, bo_handles, pitches, offsets, buf_id, flags);
}

FAKE_EXPORT int drmModeRmFB(int fd, uint32_t bufferId)
{
    FakeFb *fb = find_fb(bufferId);
    (void)fd;
    if (!fb || !fb->alive || bufferId == FB_FBCON) {
        fake_error("drmModeRmFB(%u) of a framebuffer that does not exist", bufferId);
        return -ENOENT;
    }
    fb->alive = 0;
    return 0;
}

FAKE_EXPORT drmModeFBPtr drmModeGetFB(int fd, uint32_t bufferId)
{
    (void)fd;
    (void)bufferId;
    errno = ENOENT;
    return NULL;
}

FAKE_EXPORT int drmModeSetCrtc(int fd, uint32_t crtcId, uint32_t bufferId, uint32_t x, uint32_t y,
                               uint32_t *connectors, int count, drmModeModeInfoPtr mode)
{
    (void)fd;
    (void)x;
    (void)y;
    if (crtcId != OBJ_CRTC || count != 1 || !connectors || connectors[0] != OBJ_CONNECTOR || !mode) {
        fake_error("drmModeSetCrtc with unexpected crtc/connector/mode arguments");
        return -EINVAL;
    }
    if (mode->hdisplay != g_mode.hdisplay || mode->vdisplay != g_mode.vdisplay) {
        fake_error("drmModeSetCrtc with a %ux%u mode; the panel only has %ux%u",
                   mode->hdisplay, mode->vdisplay, g_mode.hdisplay, g_mode.vdisplay);
        return -EINVAL;
    }
    return scanout(bufferId, "drmModeSetCrtc");
}

FAKE_EXPORT int drmModeSetCursor(int fd, uint32_t crtcId, uint32_t bo_handle, uint32_t width, uint32_t height)
{
    (void)fd; (void)crtcId; (void)bo_handle; (void)width; (void)height;
    return 0;
}

FAKE_EXPORT int drmModeSetCursor2(int fd, uint32_t crtcId, uint32_t bo_handle, uint32_t width, uint32_t height,
                                  int32_t hot_x, int32_t hot_y)
{
    (void)fd; (void)crtcId; (void)bo_handle; (void)width; (void)height; (void)hot_x; (void)hot_y;
    return 0;
}

FAKE_EXPORT int drmModeMoveCursor(int fd, uint32_t crtcId, int x, int y)
{
    (void)fd; (void)crtcId; (void)x; (void)y;
    return 0;
}

FAKE_EXPORT int drmHandleEvent(int fd, drmEventContextPtr evctx)
{
    if (g_pending_flip && evctx && evctx->page_flip_handler) {
        g_pending_flip = 0;
        evctx->page_flip_handler(fd, 1, 0, 0, g_pending_flip_data);
    }
    return 0;
}

FAKE_EXPORT int drmModePageFlip(int fd, uint32_t crtc_id, uint32_t fb_id, uint32_t flags, void *user_data)
{
    int ret;
    (void)fd;
    (void)flags;
    if (crtc_id != OBJ_CRTC) {
        fake_error("drmModePageFlip on crtc %u", crtc_id);
        return -EINVAL;
    }
    if (g_pending_flip) {
        fake_error("drmModePageFlip while a flip is pending");
        return -EBUSY;
    }
    ret = scanout(fb_id, "drmModePageFlip");
    if (ret == 0) {
        g_pending_flip = 1;
        g_pending_flip_data = user_data;
    }
    return ret;
}

FAKE_EXPORT int drmModeObjectSetProperty(int fd, uint32_t object_id, uint32_t object_type,
                                         uint32_t property_id, uint64_t value)
{
    (void)fd; (void)object_id; (void)object_type; (void)property_id; (void)value;
    return 0;
}

FAKE_EXPORT int drmModeSetPlane(int fd, uint32_t plane_id, uint32_t crtc_id, uint32_t fb_id, uint32_t flags,
                                int32_t crtc_x, int32_t crtc_y, uint32_t crtc_w, uint32_t crtc_h,
                                uint32_t src_x, uint32_t src_y, uint32_t src_w, uint32_t src_h)
{
    (void)fd; (void)plane_id; (void)crtc_id; (void)fb_id; (void)flags; (void)crtc_x; (void)crtc_y;
    (void)crtc_w; (void)crtc_h; (void)src_x; (void)src_y; (void)src_w; (void)src_h;
    return 0;
}

FAKE_EXPORT int drmIoctl(int fd, unsigned long request, void *arg)
{
    (void)fd; (void)request; (void)arg;
    return 0;
}

struct _drmModeAtomicReq
{
    int count;
    struct
    {
        uint32_t obj;
        uint32_t prop;
        uint64_t value;
    } items[256];
};

FAKE_EXPORT drmModeAtomicReqPtr drmModeAtomicAlloc(void)
{
    return calloc(1, sizeof(struct _drmModeAtomicReq));
}

FAKE_EXPORT void drmModeAtomicFree(drmModeAtomicReqPtr req)
{
    free(req);
}

FAKE_EXPORT int drmModeAtomicAddProperty(drmModeAtomicReqPtr req, uint32_t object_id, uint32_t property_id, uint64_t value)
{
    if (!req) {
        return -EINVAL;  // libdrm's behaviour; SDL can hit it after a mid-swap surface rebuild.
    }
    if (req->count >= (int)(sizeof(req->items) / sizeof(req->items[0]))) {
        return -ENOMEM;
    }
    req->items[req->count].obj = object_id;
    req->items[req->count].prop = property_id;
    req->items[req->count].value = value;
    return ++req->count;
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_until_ns(uint64_t when)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(when / 1000000000ull);
    ts.tv_nsec = (long)(when % 1000000000ull);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
    }
}

// FAKE_KMS_FLIP_MS: an OUT_FENCE that polls readable when the pending flip completes.
static int flip_fence_fd(void)
{
    struct itimerspec its;
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);

    if (tfd < 0) {
        fake_error("timerfd_create: %s", strerror(errno));
        return open("/dev/null", O_RDONLY | O_CLOEXEC);
    }
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = (time_t)(g_flip_done_ns / 1000000000ull);
    its.it_value.tv_nsec = (long)(g_flip_done_ns % 1000000000ull);
    if (timerfd_settime(tfd, TFD_TIMER_ABSTIME, &its, NULL) != 0) {
        fake_error("timerfd_settime: %s", strerror(errno));
    }
    g_report.flip_fences++;
    return tfd;
}

FAKE_EXPORT int drmModeAtomicCommit(int fd, const drmModeAtomicReqPtr req, uint32_t flags, void *user_data)
{
    // Later values for the same object/property win, as in libdrm.
    uint64_t fb_id = 0, src_w = 0, src_h = 0, crtc_w = 0, crtc_h = 0;
    int have_fb = 0, i;
    int *out_fence = NULL;
    (void)fd;
    (void)user_data;

    if (!req) {
        return -EINVAL;
    }
    if (!g_atomic) {
        fake_error("atomic commit without DRM_CLIENT_CAP_ATOMIC");
        return -EINVAL;
    }
    if (g_flip_ms > 0 && monotonic_ns() < g_flip_done_ns) {
        if (flags & DRM_MODE_ATOMIC_NONBLOCK) {
            // drm_atomic_helper_setup_commit(): "Userspace is not allowed to
            // get ahead of the previous commit with nonblocking ones."
            // Returned as real libdrm returns it: drmModeAtomicCommit() is
            // DRM_IOCTL(), `ret < 0 ? -errno : ret` over drmIoctl(), so the
            // result is -EBUSY and errno is left at EBUSY by the ioctl.
            g_report.busy_commits++;
            fake_error("nonblocking atomic commit while the previous flip is still pending (-EBUSY)");
            errno = EBUSY;
            return -EBUSY;
        }
        g_report.blocking_stalls++;
        sleep_until_ns(g_flip_done_ns);
    }
    for (i = 0; i < req->count; ++i) {
        const FakeProp *prop = find_prop(req->items[i].prop);
        if (!prop || prop->obj != req->items[i].obj) {
            // The kernel rejects the whole commit (-ENOENT); SDL's lookups by name can
            // pass a property id of 0 for a missing name.
            fake_error("atomic commit: property %u is not a property of object %u",
                       req->items[i].prop, req->items[i].obj);
            return -ENOENT;
        }
        switch (prop->id) {
        case PROP_FB_ID:
            fb_id = req->items[i].value;
            have_fb = 1;
            break;
        case PROP_SRC_W:
            src_w = req->items[i].value;
            break;
        case PROP_SRC_H:
            src_h = req->items[i].value;
            break;
        case PROP_CRTC_W:
            crtc_w = req->items[i].value;
            break;
        case PROP_CRTC_H:
            crtc_h = req->items[i].value;
            break;
        case PROP_OUT_FENCE_PTR:
            if (req->items[i].value) {
                out_fence = (int *)(uintptr_t)req->items[i].value;
            }
            break;
        default:
            break;
        }
    }

    if (have_fb && fb_id != 0) {
        FakeFb *fb = find_fb((uint32_t)fb_id);
        if (!fb || !fb->alive) {
            fake_error("atomic commit: plane FB_ID %llu does not exist", (unsigned long long)fb_id);
            return -ENOENT;
        }
        if ((src_w >> 16) != (uint64_t)fb->w || (src_h >> 16) != (uint64_t)fb->h) {
            fake_error("atomic commit: plane source %llux%llu does not match its %dx%d framebuffer",
                       (unsigned long long)(src_w >> 16), (unsigned long long)(src_h >> 16), fb->w, fb->h);
            return -EINVAL;
        }
        if (crtc_w != (uint64_t)g_mode.hdisplay || crtc_h != (uint64_t)g_mode.vdisplay) {
            fake_error("atomic commit: plane destination %llux%llu, the mode is %ux%u",
                       (unsigned long long)crtc_w, (unsigned long long)crtc_h, g_mode.hdisplay, g_mode.vdisplay);
            return -EINVAL;
        }
        if (scanout((uint32_t)fb_id, "atomic commit") != 0) {
            return -EINVAL;
        }
        g_report.plane_src_w = (int)(src_w >> 16);
        g_report.plane_src_h = (int)(src_h >> 16);
        g_report.plane_crtc_w = (int)crtc_w;
        g_report.plane_crtc_h = (int)crtc_h;
    }
    if (g_flip_ms > 0) {
        g_flip_done_ns = monotonic_ns() + (uint64_t)g_flip_ms * 1000000ull;
        if (!(flags & DRM_MODE_ATOMIC_NONBLOCK)) {
            // A blocking commit returns once its flip is done.
            sleep_until_ns(g_flip_done_ns);
        }
    }
    if (out_fence) {
        *out_fence = g_flip_ms > 0 ? flip_fence_fd() : open("/dev/null", O_RDONLY | O_CLOEXEC);
    }
    g_report.atomic_commits++;
    return 0;
}

FAKE_EXPORT int drmModeCreatePropertyBlob(int fd, const void *data, size_t size, uint32_t *id)
{
    (void)fd;
    (void)data;
    (void)size;
    *id = g_next_blob++;
    return 0;
}

/* ------------------------------------------------------------------ EGL -- */

typedef struct FakeConfig
{
    EGLint id, visual, r, g, b, a, depth, stencil, renderable, surface_type;
} FakeConfig;

// Mesa's GBM configs: native visual ids are GBM fourccs; smaller depth first.
static const FakeConfig g_configs[] = {
    { 1, GBM_FORMAT_ARGB8888, 8, 8, 8, 8, 0, 0, EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT_KHR, EGL_WINDOW_BIT },
    { 2, GBM_FORMAT_XRGB8888, 8, 8, 8, 0, 0, 0, EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT_KHR, EGL_WINDOW_BIT },
    { 3, GBM_FORMAT_ARGB8888, 8, 8, 8, 8, 24, 8, EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT_KHR, EGL_WINDOW_BIT },
    { 4, GBM_FORMAT_XRGB8888, 8, 8, 8, 0, 24, 8, EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT_KHR, EGL_WINDOW_BIT },
};

typedef struct FakeImage
{
    struct gbm_bo *bo;
    int alive;
} FakeImage;

typedef struct FakeTexture
{
    GLuint name;
    FakeImage *image;
} FakeTexture;

typedef struct FakeFramebuffer
{
    GLuint name;
    GLuint texture; // colour attachment 0 (a texture name), 0 for none
} FakeFramebuffer;

typedef struct FakeContext
{
    int alive;
    EGLenum api;
    GLuint next_name;
    FakeTexture textures[32];
    int num_textures;
    GLuint bound_texture;
    FakeFramebuffer framebuffers[16];
    int num_framebuffers;
    GLuint bound_framebuffer; // 0: the current draw surface
    GLint viewport[4];
    const void *attrib[2];
    GLuint program;
} FakeContext;

typedef struct FakeEglSurface
{
    int alive;
    struct gbm_surface *gs;
    const FakeConfig *config;
} FakeEglSurface;

typedef struct FakeSync
{
    EGLenum type;
    int fd;
    FakeContext *ctx;
} FakeSync;

static int g_display;
static EGLenum g_api = EGL_OPENGL_ES_API;
static EGLint g_egl_error = EGL_SUCCESS;
static FakeContext *g_ctx;
static FakeEglSurface *g_draw;

static void report_current(void)
{
    g_report.current_ctx = g_ctx;
    g_report.current_draw_surface = (g_draw && g_draw->gs) ? g_draw->gs->index : -1;
}

static EGLBoolean egl_fail(EGLint error)
{
    g_egl_error = error;
    return EGL_FALSE;
}

FAKE_EXPORT EGLint eglGetError(void)
{
    EGLint error = g_egl_error;
    g_egl_error = EGL_SUCCESS;
    return error;
}

FAKE_EXPORT EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id)
{
    (void)display_id;
    fake_init();
    return (EGLDisplay)&g_display;
}

FAKE_EXPORT EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native_display, const EGLAttrib *attrib_list)
{
    (void)native_display;
    (void)attrib_list;
    fake_init();
    if (platform != EGL_PLATFORM_GBM_KHR) {
        fake_error("eglGetPlatformDisplay for platform 0x%x", platform);
        return EGL_NO_DISPLAY;
    }
    return (EGLDisplay)&g_display;
}

FAKE_EXPORT EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor)
{
    (void)dpy;
    if (major) {
        *major = 1;
    }
    if (minor) {
        *minor = 5;
    }
    return EGL_TRUE;
}

FAKE_EXPORT EGLBoolean eglTerminate(EGLDisplay dpy)
{
    (void)dpy;
    return EGL_TRUE;
}

FAKE_EXPORT const char *eglQueryString(EGLDisplay dpy, EGLint name)
{
    fake_init();
    switch (name) {
    case EGL_VERSION:
        return "1.5 fake-kms";
    case EGL_VENDOR:
        return "fake-kms";
    case EGL_CLIENT_APIS:
        return "OpenGL_ES";
    case EGL_EXTENSIONS:
        if (dpy == EGL_NO_DISPLAY) {
            return "EGL_EXT_platform_base EGL_KHR_platform_gbm EGL_MESA_platform_gbm";
        }
        return g_display_extensions;
    default:
        g_egl_error = EGL_BAD_PARAMETER;
        return NULL;
    }
}

FAKE_EXPORT EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs,
                                       EGLint config_size, EGLint *num_config)
{
    EGLint count = 0;
    size_t c;
    (void)dpy;

    for (c = 0; c < sizeof(g_configs) / sizeof(g_configs[0]); ++c) {
        const FakeConfig *cfg = &g_configs[c];
        const EGLint *a;
        int ok = 1;

        for (a = attrib_list; a && a[0] != EGL_NONE && ok; a += 2) {
            const EGLint want = a[1];
            switch (a[0]) {
            case EGL_RED_SIZE:     ok = want == EGL_DONT_CARE || cfg->r >= want; break;
            case EGL_GREEN_SIZE:   ok = want == EGL_DONT_CARE || cfg->g >= want; break;
            case EGL_BLUE_SIZE:    ok = want == EGL_DONT_CARE || cfg->b >= want; break;
            case EGL_ALPHA_SIZE:   ok = want == EGL_DONT_CARE || cfg->a >= want; break;
            case EGL_DEPTH_SIZE:   ok = want == EGL_DONT_CARE || cfg->depth >= want; break;
            case EGL_STENCIL_SIZE: ok = want == EGL_DONT_CARE || cfg->stencil >= want; break;
            case EGL_SURFACE_TYPE: ok = want == EGL_DONT_CARE || (cfg->surface_type & want) == want; break;
            case EGL_RENDERABLE_TYPE: ok = want == EGL_DONT_CARE || (cfg->renderable & want) == want; break;
            default: break;
            }
        }
        if (!ok) {
            continue;
        }
        if (configs && count < config_size) {
            configs[count] = (EGLConfig)cfg;
        }
        ++count;
    }
    if (configs && count > config_size) {
        count = config_size;
    }
    *num_config = count;
    return EGL_TRUE;
}

FAKE_EXPORT EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value)
{
    const FakeConfig *cfg = (const FakeConfig *)config;
    (void)dpy;
    switch (attribute) {
    case EGL_CONFIG_ID: *value = cfg->id; break;
    case EGL_NATIVE_VISUAL_ID: *value = cfg->visual; break;
    case EGL_RED_SIZE: *value = cfg->r; break;
    case EGL_GREEN_SIZE: *value = cfg->g; break;
    case EGL_BLUE_SIZE: *value = cfg->b; break;
    case EGL_ALPHA_SIZE: *value = cfg->a; break;
    case EGL_BUFFER_SIZE: *value = cfg->r + cfg->g + cfg->b + cfg->a; break;
    case EGL_DEPTH_SIZE: *value = cfg->depth; break;
    case EGL_STENCIL_SIZE: *value = cfg->stencil; break;
    case EGL_RENDERABLE_TYPE: *value = cfg->renderable; break;
    case EGL_SURFACE_TYPE: *value = cfg->surface_type; break;
    case EGL_CONFIG_CAVEAT: *value = EGL_NONE; break;
    default: *value = 0; break;
    }
    return EGL_TRUE;
}

FAKE_EXPORT EGLBoolean eglBindAPI(EGLenum api)
{
    if (api != EGL_OPENGL_ES_API && api != EGL_OPENGL_API) {
        return egl_fail(EGL_BAD_PARAMETER);
    }
    g_api = api;
    return EGL_TRUE;
}

FAKE_EXPORT EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context,
                                        const EGLint *attrib_list)
{
    FakeContext *ctx;
    (void)dpy;
    (void)config;
    (void)share_context;
    (void)attrib_list;
    ctx = calloc(1, sizeof(*ctx));
    ctx->alive = 1;
    ctx->api = g_api;
    ctx->next_name = 1;
    g_report.contexts_created++;
    g_report.contexts_alive++;
    return (EGLContext)ctx;
}

FAKE_EXPORT EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext context)
{
    FakeContext *ctx = (FakeContext *)context;
    (void)dpy;
    if (!ctx || !ctx->alive) {
        fake_error("eglDestroyContext on a dead context");
        return egl_fail(EGL_BAD_CONTEXT);
    }
    if (ctx == g_ctx) {
        fake_error("eglDestroyContext on the current context");
    }
    ctx->alive = 0;
    g_report.contexts_alive--;
    return EGL_TRUE;
}

FAKE_EXPORT EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win,
                                              const EGLint *attrib_list)
{
    const FakeConfig *cfg = (const FakeConfig *)config;
    struct gbm_surface *gs = (struct gbm_surface *)(uintptr_t)win;
    FakeEglSurface *surface;
    (void)dpy;
    (void)attrib_list;

    if (!gs || !gs->alive) {
        fake_error("eglCreateWindowSurface on a dead gbm surface");
        g_egl_error = EGL_BAD_NATIVE_WINDOW;
        return EGL_NO_SURFACE;
    }
    // Mesa platform_drm: the config's visual must be the gbm surface's format.
    if ((uint32_t)cfg->visual != gs->format) {
        fake_error("eglCreateWindowSurface: config visual 0x%x does not match gbm surface format 0x%x",
                   (unsigned)cfg->visual, gs->format);
        g_egl_error = EGL_BAD_MATCH;
        return EGL_NO_SURFACE;
    }
    surface = calloc(1, sizeof(*surface));
    surface->alive = 1;
    surface->gs = gs;
    surface->config = cfg;
    g_report.egl_surfaces_alive++;
    return (EGLSurface)surface;
}

FAKE_EXPORT EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list)
{
    (void)dpy;
    (void)config;
    (void)attrib_list;
    g_egl_error = EGL_BAD_MATCH;
    return EGL_NO_SURFACE;
}

FAKE_EXPORT EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface)
{
    FakeEglSurface *s = (FakeEglSurface *)surface;
    (void)dpy;
    if (!s || !s->alive) {
        fake_error("eglDestroySurface on a dead surface");
        return egl_fail(EGL_BAD_SURFACE);
    }
    if (s == g_draw) {
        fake_error("eglDestroySurface on the current draw surface");
    }
    s->alive = 0;
    g_report.egl_surfaces_alive--;
    return EGL_TRUE;
}

FAKE_EXPORT EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext context)
{
    FakeContext *ctx = (FakeContext *)context;
    FakeEglSurface *d = (FakeEglSurface *)draw;
    (void)dpy;

    if (draw != read) {
        fake_error("eglMakeCurrent with different draw and read surfaces");
        return egl_fail(EGL_BAD_MATCH);
    }
    if (!ctx) {
        if (d) {
            return egl_fail(EGL_BAD_MATCH);
        }
        g_ctx = NULL;
        g_draw = NULL;
        report_current();
        return EGL_TRUE;
    }
    if (!ctx->alive || (d && !d->alive)) {
        fake_error("eglMakeCurrent with a dead context or surface");
        return egl_fail(EGL_BAD_CONTEXT);
    }
    if (d && d->gs && d->gs->index == g_fail_makecurrent_surface && !g_report.injected_makecurrent_failures) {
        // Injected (FAKE_EGL_FAIL_MAKECURRENT_SURFACE): the current state is unchanged, as in EGL.
        g_report.injected_makecurrent_failures++;
        return egl_fail(EGL_BAD_ALLOC);
    }
    g_ctx = ctx;
    g_draw = d;
    report_current();
    return EGL_TRUE;
}

FAKE_EXPORT EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface)
{
    FakeEglSurface *s = (FakeEglSurface *)surface;
    struct gbm_surface *gs;
    int i, next = -1;
    (void)dpy;

    if (!s || !s->alive) {
        fake_error("eglSwapBuffers on a dead surface");
        return egl_fail(EGL_BAD_SURFACE);
    }
    if (!g_ctx || s != g_draw) {
        fake_error("eglSwapBuffers on a surface that is not the current draw surface");
        return egl_fail(EGL_BAD_SURFACE);
    }
    gs = s->gs;
    if (!gs->alive) {
        fake_error("eglSwapBuffers on a surface whose gbm surface is gone");
        return egl_fail(EGL_BAD_NATIVE_WINDOW);
    }

    g_report.surfaces[gs->index].swaps++;
    gs->bos[gs->back].frame = g_report.surfaces[gs->index].swaps;
    gs->front_pending = gs->back;  // an unlocked previous front is simply dropped
    for (i = 0; i < FAKE_KMS_BOS_PER_SURFACE; ++i) {
        if (i != gs->front_pending && !gs->bos[i].locked) {
            next = i;
            break;
        }
    }
    if (next < 0) {
        fake_error("eglSwapBuffers: surface %d (%ux%u) has no free buffer left (buffers are leaking)",
                   gs->index, gs->w, gs->h);
        return egl_fail(EGL_BAD_ALLOC);
    }
    gs->back = next;
    report_surface(gs);
    return EGL_TRUE;
}

FAKE_EXPORT EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval)
{
    (void)dpy;
    (void)interval;
    return EGL_TRUE;
}

FAKE_EXPORT EGLBoolean eglWaitNative(EGLint engine)
{
    (void)engine;
    return EGL_TRUE;
}

FAKE_EXPORT EGLBoolean eglWaitGL(void)
{
    return EGL_TRUE;
}

FAKE_EXPORT EGLSyncKHR eglCreateSyncKHR(EGLDisplay dpy, EGLenum type, const EGLint *attrib_list)
{
    FakeSync *sync;
    const EGLint *a;
    (void)dpy;

    if (!g_ctx) {
        fake_error("eglCreateSyncKHR with no current context");
        g_egl_error = EGL_BAD_MATCH;
        return EGL_NO_SYNC_KHR;
    }
    if ((type == EGL_SYNC_FENCE_KHR && !g_fence_sync) ||
        (type == EGL_SYNC_NATIVE_FENCE_ANDROID && !g_native_fence) ||
        (type != EGL_SYNC_FENCE_KHR && type != EGL_SYNC_NATIVE_FENCE_ANDROID)) {
        fake_error("eglCreateSyncKHR of type 0x%x that the display does not advertise", type);
        g_egl_error = EGL_BAD_ATTRIBUTE;
        return EGL_NO_SYNC_KHR;
    }
    sync = calloc(1, sizeof(*sync));
    sync->type = type;
    sync->fd = -1;
    sync->ctx = g_ctx;
    for (a = attrib_list; a && a[0] != EGL_NONE; a += 2) {
        if (a[0] == EGL_SYNC_NATIVE_FENCE_FD_ANDROID) {
            sync->fd = a[1];
        }
    }
    g_report.syncs_alive++;
    return (EGLSyncKHR)sync;
}

FAKE_EXPORT EGLBoolean eglDestroySyncKHR(EGLDisplay dpy, EGLSyncKHR sync)
{
    FakeSync *s = (FakeSync *)sync;
    (void)dpy;
    if (!s) {
        fake_error("eglDestroySyncKHR on no sync");
        return egl_fail(EGL_BAD_PARAMETER);
    }
    if (s->fd >= 0) {
        close(s->fd);
    }
    free(s);
    g_report.syncs_alive--;
    return EGL_TRUE;
}

FAKE_EXPORT EGLint eglClientWaitSyncKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags, EGLTimeKHR timeout)
{
    (void)dpy;
    (void)flags;
    (void)timeout;
    if (!sync) {
        fake_error("eglClientWaitSyncKHR on no sync");
        return EGL_FALSE;
    }
    g_report.client_waits++;
    return EGL_CONDITION_SATISFIED_KHR;
}

FAKE_EXPORT EGLint eglWaitSyncKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags)
{
    FakeSync *s = (FakeSync *)sync;
    (void)dpy;
    (void)flags;
    if (!s || !g_ctx) {
        fake_error("eglWaitSyncKHR with no sync or no current context");
        return EGL_FALSE;
    }
    if (s->ctx != g_ctx) {
        g_report.cross_context_waits++;
    }
    return EGL_TRUE;
}

FAKE_EXPORT EGLint eglDupNativeFenceFDANDROID(EGLDisplay dpy, EGLSyncKHR sync)
{
    FakeSync *s = (FakeSync *)sync;
    (void)dpy;
    if (!s || s->type != EGL_SYNC_NATIVE_FENCE_ANDROID) {
        fake_error("eglDupNativeFenceFDANDROID on a non-native fence");
        return EGL_NO_NATIVE_FENCE_FD_ANDROID;
    }
    return s->fd >= 0 ? dup(s->fd) : open("/dev/null", O_RDONLY | O_CLOEXEC);
}

FAKE_EXPORT EGLImageKHR eglCreateImageKHR(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer,
                                          const EGLint *attrib_list)
{
    struct gbm_bo *bo = (struct gbm_bo *)buffer;
    FakeImage *image;
    (void)dpy;
    (void)attrib_list;

    if (target != EGL_NATIVE_PIXMAP_KHR || ctx != EGL_NO_CONTEXT || !bo || bo->destroyed) {
        fake_error("eglCreateImageKHR: expected EGL_NATIVE_PIXMAP_KHR of a live gbm_bo with no context");
        g_egl_error = EGL_BAD_PARAMETER;
        return EGL_NO_IMAGE_KHR;
    }
    image = calloc(1, sizeof(*image));
    image->bo = bo;
    image->alive = 1;
    g_report.images_created++;
    g_report.images_alive++;
    return (EGLImageKHR)image;
}

FAKE_EXPORT EGLBoolean eglDestroyImageKHR(EGLDisplay dpy, EGLImageKHR image)
{
    FakeImage *img = (FakeImage *)image;
    (void)dpy;
    if (!img || !img->alive) {
        fake_error("eglDestroyImageKHR on a dead image");
        return egl_fail(EGL_BAD_PARAMETER);
    }
    img->alive = 0;
    g_report.images_alive--;
    return EGL_TRUE;
}

/* ------------------------------------------------------------------- GL -- */

static FakeContext *gl_ctx(const char *fn)
{
    if (!g_ctx) {
        fake_error("%s with no current context", fn);
    }
    return g_ctx;
}

static FakeTexture *find_texture(FakeContext *ctx, GLuint name)
{
    int i;
    for (i = 0; i < ctx->num_textures; ++i) {
        if (ctx->textures[i].name == name) {
            return &ctx->textures[i];
        }
    }
    return NULL;
}

FAKE_EXPORT void glActiveTexture(GLenum texture) { (void)texture; gl_ctx("glActiveTexture"); }
FAKE_EXPORT void glAttachShader(GLuint program, GLuint shader) { (void)program; (void)shader; gl_ctx("glAttachShader"); }
FAKE_EXPORT void glBindAttribLocation(GLuint program, GLuint index, const GLchar *name) { (void)program; (void)index; (void)name; gl_ctx("glBindAttribLocation"); }
FAKE_EXPORT void glClear(GLbitfield mask)
{
    FakeContext *ctx = gl_ctx("glClear");
    (void)mask;
    if (ctx && !ctx->bound_framebuffer && g_draw && g_draw->gs) {
        g_report.surfaces[g_draw->gs->index].clears++;
    }
}
FAKE_EXPORT void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { (void)r; (void)g; (void)b; (void)a; gl_ctx("glClearColor"); }
FAKE_EXPORT void glCompileShader(GLuint shader) { (void)shader; gl_ctx("glCompileShader"); }
FAKE_EXPORT void glDeleteProgram(GLuint program) { (void)program; gl_ctx("glDeleteProgram"); }
FAKE_EXPORT void glDeleteShader(GLuint shader) { (void)shader; gl_ctx("glDeleteShader"); }
FAKE_EXPORT void glDisable(GLenum cap) { (void)cap; gl_ctx("glDisable"); }
FAKE_EXPORT void glEnable(GLenum cap) { (void)cap; gl_ctx("glEnable"); }
FAKE_EXPORT void glEnableVertexAttribArray(GLuint index) { (void)index; gl_ctx("glEnableVertexAttribArray"); }
FAKE_EXPORT void glFinish(void) { gl_ctx("glFinish"); }
FAKE_EXPORT void glFlush(void) { gl_ctx("glFlush"); }
#ifndef FAKE_KMS_OMIT_GL_LINK_PROGRAM
FAKE_EXPORT void glLinkProgram(GLuint program) { (void)program; gl_ctx("glLinkProgram"); }
#endif
FAKE_EXPORT void glPixelStorei(GLenum pname, GLint param) { (void)pname; (void)param; gl_ctx("glPixelStorei"); }
FAKE_EXPORT void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length) { (void)shader; (void)count; (void)string; (void)length; gl_ctx("glShaderSource"); }
FAKE_EXPORT void glTexParameteri(GLenum target, GLenum pname, GLint param) { (void)target; (void)pname; (void)param; gl_ctx("glTexParameteri"); }
FAKE_EXPORT void glUniform1i(GLint location, GLint v0) { (void)location; (void)v0; gl_ctx("glUniform1i"); }
FAKE_EXPORT GLenum glGetError(void) { gl_ctx("glGetError"); return GL_NO_ERROR; }
FAKE_EXPORT GLint glGetUniformLocation(GLuint program, const GLchar *name) { (void)program; (void)name; gl_ctx("glGetUniformLocation"); return 0; }

FAKE_EXPORT GLuint glCreateShader(GLenum type)
{
    FakeContext *ctx = gl_ctx("glCreateShader");
    (void)type;
    return ctx ? ctx->next_name++ : 0;
}

FAKE_EXPORT GLuint glCreateProgram(void)
{
    FakeContext *ctx = gl_ctx("glCreateProgram");
    return ctx ? ctx->next_name++ : 0;
}

FAKE_EXPORT void glUseProgram(GLuint program)
{
    FakeContext *ctx = gl_ctx("glUseProgram");
    if (ctx) {
        ctx->program = program;
    }
}

FAKE_EXPORT void glGetShaderiv(GLuint shader, GLenum pname, GLint *params)
{
    (void)shader;
    gl_ctx("glGetShaderiv");
    *params = pname == GL_COMPILE_STATUS ? GL_TRUE : 0;
}

FAKE_EXPORT void glGetProgramiv(GLuint program, GLenum pname, GLint *params)
{
    (void)program;
    gl_ctx("glGetProgramiv");
    *params = pname == GL_LINK_STATUS ? GL_TRUE : 0;
}

FAKE_EXPORT void glGetIntegerv(GLenum pname, GLint *data)
{
    gl_ctx("glGetIntegerv");
    *data = pname == GL_MAX_TEXTURE_SIZE ? 4096 : 0;
}

FAKE_EXPORT const GLubyte *glGetString(GLenum name)
{
    gl_ctx("glGetString");
    switch (name) {
    case GL_VERSION:
        return (const GLubyte *)"OpenGL ES 2.0 fake-kms";
    case GL_EXTENSIONS:
        return (const GLubyte *)"GL_OES_EGL_image GL_OES_surfaceless_context";
    case GL_SHADING_LANGUAGE_VERSION:
        return (const GLubyte *)"OpenGL ES GLSL ES 1.00";
    default:
        return (const GLubyte *)"fake-kms";
    }
}

FAKE_EXPORT void glGenTextures(GLsizei n, GLuint *textures)
{
    FakeContext *ctx = gl_ctx("glGenTextures");
    GLsizei i;
    for (i = 0; i < n; ++i) {
        textures[i] = 0;
        if (ctx && ctx->num_textures < (int)(sizeof(ctx->textures) / sizeof(ctx->textures[0]))) {
            ctx->textures[ctx->num_textures].name = ctx->next_name++;
            ctx->textures[ctx->num_textures].image = NULL;
            textures[i] = ctx->textures[ctx->num_textures].name;
            ctx->num_textures++;
        }
    }
}

FAKE_EXPORT void glDeleteTextures(GLsizei n, const GLuint *textures)
{
    FakeContext *ctx = gl_ctx("glDeleteTextures");
    GLsizei i;
    for (i = 0; ctx && i < n; ++i) {
        FakeTexture *tex = find_texture(ctx, textures[i]);
        if (tex) {
            tex->name = 0;
            tex->image = NULL;
        }
    }
}

FAKE_EXPORT void glBindTexture(GLenum target, GLuint texture)
{
    FakeContext *ctx = gl_ctx("glBindTexture");
    (void)target;
    if (ctx) {
        ctx->bound_texture = texture;
    }
}

FAKE_EXPORT void glEGLImageTargetTexture2DOES(GLenum target, GLeglImageOES image)
{
    FakeContext *ctx = gl_ctx("glEGLImageTargetTexture2DOES");
    FakeTexture *tex;
    FakeImage *img = (FakeImage *)image;

    if (!ctx) {
        return;
    }
    tex = find_texture(ctx, ctx->bound_texture);
    if (target != GL_TEXTURE_2D || !tex || !img || !img->alive) {
        fake_error("glEGLImageTargetTexture2DOES without a bound texture or a live image");
        return;
    }
    tex->image = img;
}

FAKE_EXPORT void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    FakeContext *ctx = gl_ctx("glViewport");
    if (ctx) {
        ctx->viewport[0] = x;
        ctx->viewport[1] = y;
        ctx->viewport[2] = width;
        ctx->viewport[3] = height;
    }
}

FAKE_EXPORT void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                                       GLsizei stride, const void *pointer)
{
    FakeContext *ctx = gl_ctx("glVertexAttribPointer");
    (void)normalized;
    if (!ctx) {
        return;
    }
    if (index > 1 || size != 2 || type != GL_FLOAT || stride != 0) {
        fake_error("glVertexAttribPointer(%u, %d, 0x%x, stride %d): the fake models packed vec2 attributes 0 and 1",
                   index, size, type, stride);
        return;
    }
    ctx->attrib[index] = pointer;
}

static FakeFramebuffer *find_framebuffer(FakeContext *ctx, GLuint name)
{
    int i;
    for (i = 0; name && i < ctx->num_framebuffers; ++i) {
        if (ctx->framebuffers[i].name == name) {
            return &ctx->framebuffers[i];
        }
    }
    return NULL;
}

FAKE_EXPORT void glGenFramebuffers(GLsizei n, GLuint *framebuffers)
{
    FakeContext *ctx = gl_ctx("glGenFramebuffers");
    GLsizei i;
    for (i = 0; i < n; ++i) {
        framebuffers[i] = 0;
        if (ctx && ctx->num_framebuffers < (int)(sizeof(ctx->framebuffers) / sizeof(ctx->framebuffers[0]))) {
            ctx->framebuffers[ctx->num_framebuffers].name = ctx->next_name++;
            ctx->framebuffers[ctx->num_framebuffers].texture = 0;
            framebuffers[i] = ctx->framebuffers[ctx->num_framebuffers].name;
            ctx->num_framebuffers++;
        }
    }
}

FAKE_EXPORT void glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers)
{
    FakeContext *ctx = gl_ctx("glDeleteFramebuffers");
    GLsizei i;
    for (i = 0; ctx && i < n; ++i) {
        FakeFramebuffer *fb = find_framebuffer(ctx, framebuffers[i]);
        if (fb) {
            if (ctx->bound_framebuffer == fb->name) {
                ctx->bound_framebuffer = 0;
            }
            fb->name = 0;
            fb->texture = 0;
        }
    }
}

FAKE_EXPORT void glBindFramebuffer(GLenum target, GLuint framebuffer)
{
    FakeContext *ctx = gl_ctx("glBindFramebuffer");
    if (!ctx) {
        return;
    }
    if (target != GL_FRAMEBUFFER || (framebuffer && !find_framebuffer(ctx, framebuffer))) {
        fake_error("glBindFramebuffer(0x%x, %u): not a framebuffer of this context", target, framebuffer);
        return;
    }
    ctx->bound_framebuffer = framebuffer;
}

FAKE_EXPORT void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
{
    FakeContext *ctx = gl_ctx("glFramebufferTexture2D");
    FakeFramebuffer *fb;
    if (!ctx) {
        return;
    }
    fb = find_framebuffer(ctx, ctx->bound_framebuffer);
    if (target != GL_FRAMEBUFFER || attachment != GL_COLOR_ATTACHMENT0 || textarget != GL_TEXTURE_2D || level != 0 ||
        !fb || !find_texture(ctx, texture)) {
        fake_error("glFramebufferTexture2D: the fake models a 2D texture on colour attachment 0 of a bound framebuffer");
        return;
    }
    fb->texture = texture;
}

FAKE_EXPORT GLenum glCheckFramebufferStatus(GLenum target)
{
    FakeContext *ctx = gl_ctx("glCheckFramebufferStatus");
    FakeFramebuffer *fb;
    FakeTexture *tex;
    if (!ctx || target != GL_FRAMEBUFFER) {
        return 0;
    }
    fb = find_framebuffer(ctx, ctx->bound_framebuffer);
    tex = fb ? find_texture(ctx, fb->texture) : NULL;
    return (tex && tex->image) ? GL_FRAMEBUFFER_COMPLETE : GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
}

FAKE_EXPORT void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                              GLint border, GLenum format, GLenum type, const void *pixels)
{
    FakeContext *ctx = gl_ctx("glTexImage2D");
    FakeTexture *tex;
    (void)internalformat;
    (void)format;
    (void)type;
    if (!ctx) {
        return;
    }
    tex = find_texture(ctx, ctx->bound_texture);
    if (target != GL_TEXTURE_2D || level != 0 || border != 0 || width <= 0 || height <= 0 || pixels || !tex) {
        fake_error("glTexImage2D: the fake models storage (no pixels) for a bound 2D texture");
        return;
    }
    tex->image = NULL; // plain storage, not an EGLImage
}

/* The read framebuffer's image becomes what the bound texture samples: a model
   of copying the application's frame into a texture the rotate context owns. */
FAKE_EXPORT void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
                                     GLsizei width, GLsizei height)
{
    FakeContext *ctx = gl_ctx("glCopyTexSubImage2D");
    FakeFramebuffer *fb;
    FakeTexture *src, *dst;
    if (!ctx) {
        return;
    }
    fb = find_framebuffer(ctx, ctx->bound_framebuffer);
    src = fb ? find_texture(ctx, fb->texture) : NULL;
    dst = find_texture(ctx, ctx->bound_texture);
    if (target != GL_TEXTURE_2D || level != 0 || xoffset || yoffset || x || y || !src || !src->image || !dst ||
        dst == src || width != (GLsizei)src->image->bo->w || height != (GLsizei)src->image->bo->h) {
        fake_error("glCopyTexSubImage2D: the fake models a whole-image copy from a bound framebuffer's EGLImage");
        return;
    }
    dst->image = src->image;
    g_report.copies++;
}

FAKE_EXPORT void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    FakeContext *ctx = gl_ctx("glDrawArrays");
    FakeTexture *tex;
    FakeKmsDraw *draw = &g_report.last_draw;

    if (!ctx) {
        return;
    }
    if (ctx->bound_framebuffer) {
        fake_error("glDrawArrays with framebuffer object %u bound: the rotate pass must draw into the present surface",
                   ctx->bound_framebuffer);
        return;
    }
    tex = find_texture(ctx, ctx->bound_texture);
    if (mode != GL_TRIANGLE_STRIP || first != 0 || count != 4 || !ctx->attrib[0] || !ctx->attrib[1] ||
        !tex || !tex->image) {
        fake_error("glDrawArrays: the fake models the rotate pass (a 4-vertex strip sampling an EGLImage)");
        return;
    }
    memset(draw, 0, sizeof(*draw));
    draw->ctx = ctx;
    draw->target_surface = (g_draw && g_draw->gs) ? g_draw->gs->index : -1;
    draw->source_surface = tex->image->bo->surface;
    draw->source_w = (int)tex->image->bo->w;
    draw->source_h = (int)tex->image->bo->h;
    draw->source_frame = tex->image->bo->frame;
    draw->source_locked = tex->image->bo->locked;
    memcpy(draw->viewport, ctx->viewport, sizeof(draw->viewport));
    memcpy(draw->pos, ctx->attrib[0], sizeof(draw->pos));
    memcpy(draw->uv, ctx->attrib[1], sizeof(draw->uv));
    g_report.draws++;
    if (!draw->source_locked) {
        fake_error("rotate pass sampled a buffer the application surface may render into (not locked)");
    }
}

/* --------------------------------------------------------------- Vulkan -- */

// VkExtensionProperties: char extensionName[VK_MAX_EXTENSION_NAME_SIZE = 256]; uint32_t specVersion.
typedef struct FakeVkExtensionProperties
{
    char extensionName[256];
    uint32_t specVersion;
} FakeVkExtensionProperties;

typedef void (*FakeVkVoidFunction)(void);

static int32_t fake_vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count,
                                                           FakeVkExtensionProperties *properties)
{
    static const char *const names[] = { "VK_KHR_surface", "VK_KHR_display" };
    const uint32_t total = 2;
    uint32_t i, n;

    g_report.vulkan_enumerations++;
    if (layer) {
        return -6; // VK_ERROR_LAYER_NOT_PRESENT
    }
    if (!properties) {
        *count = total;
        return 0; // VK_SUCCESS
    }
    n = *count < total ? *count : total;
    for (i = 0; i < n; ++i) {
        memset(&properties[i], 0, sizeof(properties[i]));
        snprintf(properties[i].extensionName, sizeof(properties[i].extensionName), "%s", names[i]);
        properties[i].specVersion = 1;
    }
    *count = n;
    return n < total ? 5 : 0; // VK_INCOMPLETE : VK_SUCCESS
}

FAKE_EXPORT FakeVkVoidFunction vkGetInstanceProcAddr(void *instance, const char *name)
{
    fake_init();
    if (!instance && name && strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0) {
        return (FakeVkVoidFunction)fake_vkEnumerateInstanceExtensionProperties;
    }
    return NULL;
}

/* --------------------------------------------------------- proc address -- */

typedef void (*FakeProc)(void);

#define FAKE_PROC(name) { #name, (FakeProc)name }
static const struct
{
    const char *name;
    FakeProc proc;
} g_procs[] = {
    FAKE_PROC(eglGetError), FAKE_PROC(eglGetDisplay), FAKE_PROC(eglGetPlatformDisplay), FAKE_PROC(eglInitialize),
    FAKE_PROC(eglTerminate), FAKE_PROC(eglQueryString), FAKE_PROC(eglChooseConfig), FAKE_PROC(eglGetConfigAttrib),
    FAKE_PROC(eglBindAPI), FAKE_PROC(eglCreateContext), FAKE_PROC(eglDestroyContext), FAKE_PROC(eglCreateWindowSurface),
    FAKE_PROC(eglCreatePbufferSurface), FAKE_PROC(eglDestroySurface), FAKE_PROC(eglMakeCurrent), FAKE_PROC(eglSwapBuffers),
    FAKE_PROC(eglSwapInterval), FAKE_PROC(eglWaitNative), FAKE_PROC(eglWaitGL), FAKE_PROC(eglCreateSyncKHR),
    FAKE_PROC(eglDestroySyncKHR), FAKE_PROC(eglClientWaitSyncKHR), FAKE_PROC(eglWaitSyncKHR),
    FAKE_PROC(eglDupNativeFenceFDANDROID), FAKE_PROC(eglCreateImageKHR), FAKE_PROC(eglDestroyImageKHR),
    FAKE_PROC(glActiveTexture), FAKE_PROC(glAttachShader), FAKE_PROC(glBindAttribLocation), FAKE_PROC(glBindTexture),
    FAKE_PROC(glClear), FAKE_PROC(glClearColor), FAKE_PROC(glCompileShader), FAKE_PROC(glCreateProgram),
    FAKE_PROC(glCreateShader), FAKE_PROC(glDeleteProgram), FAKE_PROC(glDeleteShader), FAKE_PROC(glDeleteTextures),
    FAKE_PROC(glDisable), FAKE_PROC(glEnable), FAKE_PROC(glDrawArrays), FAKE_PROC(glEnableVertexAttribArray),
    FAKE_PROC(glFinish), FAKE_PROC(glFlush), FAKE_PROC(glGenTextures), FAKE_PROC(glGetError), FAKE_PROC(glGetIntegerv),
    FAKE_PROC(glGetProgramiv), FAKE_PROC(glGetShaderiv), FAKE_PROC(glGetString), FAKE_PROC(glGetUniformLocation),
#ifndef FAKE_KMS_OMIT_GL_LINK_PROGRAM
    FAKE_PROC(glLinkProgram),
#endif
    FAKE_PROC(glPixelStorei), FAKE_PROC(glShaderSource), FAKE_PROC(glTexParameteri),
    FAKE_PROC(glUniform1i), FAKE_PROC(glUseProgram), FAKE_PROC(glVertexAttribPointer), FAKE_PROC(glViewport),
    FAKE_PROC(glEGLImageTargetTexture2DOES),
    FAKE_PROC(glBindFramebuffer), FAKE_PROC(glCheckFramebufferStatus), FAKE_PROC(glCopyTexSubImage2D),
    FAKE_PROC(glDeleteFramebuffers), FAKE_PROC(glFramebufferTexture2D), FAKE_PROC(glGenFramebuffers),
    FAKE_PROC(glTexImage2D),
};

// Entry points only an SDL_KMSDRM_ROTATE_EXPERIMENT resolves (glClear is also the application's).
static int is_experiment_proc(const char *procname)
{
    static const char *const names[] = {
        "glBindFramebuffer", "glCheckFramebufferStatus", "glCopyTexSubImage2D", "glDeleteFramebuffers",
        "glFramebufferTexture2D", "glGenFramebuffers", "glTexImage2D"
    };
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcmp(names[i], procname) == 0) {
            return 1;
        }
    }
    return 0;
}

FAKE_EXPORT __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *procname)
{
    size_t i;
#ifdef FAKE_KMS_OMIT_GL_LINK_PROGRAM
    if (strcmp(procname, "glLinkProgram") == 0) {
        g_report.omitted_proc_requests++;
        return NULL;
    }
#endif
    if (is_experiment_proc(procname)) {
        g_report.experiment_proc_requests++;
    }
    for (i = 0; i < sizeof(g_procs) / sizeof(g_procs[0]); ++i) {
        if (strcmp(g_procs[i].name, procname) == 0) {
            return (__eglMustCastToProperFunctionPointerType)g_procs[i].proc;
        }
    }
    return NULL;
}
