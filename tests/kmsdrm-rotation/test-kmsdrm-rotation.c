/*
 * tsp-mc9m.41.924.16.12: an SDL application on SDL's real KMSDRM backend,
 * driving the fake 720x1280 DSI panel of fake-kms.c (libdrm/libgbm/libEGL/
 * libGLESv2 all resolve to it through LD_LIBRARY_PATH).
 *
 * Environment (set by run.sh per scenario):
 *   EXPECT_ROTATION    degrees SDL must apply at present (0 = no rotated present)
 *   EXPECT_PANEL_PROP  SDL_PROP_DISPLAY_KMSDRM_PANEL_ORIENTATION_NUMBER expected
 *                      (the true panel orientation). A GL window must report
 *                      present_rotation = EXPECT_ROTATION and app_rotation =
 *                      EXPECT_PANEL_PROP - EXPECT_ROTATION (mod 360).
 *   EXPECT_VULKAN_WINDOW 1: create a Vulkan window instead (the fake is also
 *                      libvulkan.so.1). It stays panel-native, so it must report
 *                      present_rotation 0 and app_rotation = EXPECT_PANEL_PROP.
 *   EXPECT_MAKECURRENT_FAIL 1: FAKE_EGL_FAIL_MAKECURRENT_SURFACE makes the final
 *                      eglMakeCurrent of the frame-1 surface rebuild fail; the
 *                      swap must fail with nothing leaked
 *   EXPECT_WINDOW_FAIL 1: the stack lacks a GL entry point in the middle of the
 *                      rotate pass's table (fake built with
 *                      -DFAKE_KMS_OMIT_GL_LINK_PROGRAM), so creating the GL
 *                      window must fail cleanly: no call through a missing
 *                      entry point, and nothing left alive or locked
 *   FAKE_KMS_ATOMIC, FAKE_EGL_FENCE_SYNC, FAKE_EGL_NATIVE_FENCE,
 *   FAKE_KMS_FLIP_MS   (read by the fake too). With FAKE_KMS_FLIP_MS > 0 every
 *                      flip completes that long after its commit, and SDL must
 *                      wait for it before the next nonblocking commit even
 *                      though a CPU wait on the imported EGL fence returns at
 *                      once (Zink): no commit may fail with -EBUSY
 *
 * Checks what an application sees (display mode and window size: 1280x720 for
 * a panel reporting Left/Right Side Up), what reaches the panel (a 720x1280
 * buffer every time), the rotate pass itself (its own context, the frame just
 * swapped, the exact corner mapping), and the negative control: with Normal
 * (or no) orientation nothing extra is created and the application's own
 * surface is scanned out.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gbm.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengles2.h>

#include "fake-kms.h"

static int failures;

#define CHECK(cond, ...)                 \
    do {                                 \
        if (!(cond)) {                   \
            ++failures;                  \
            printf("FAIL: ");            \
        } else {                         \
            printf("ok: ");              \
        }                                \
        printf(__VA_ARGS__);             \
        printf("\n");                    \
    } while (0)

static int env_int(const char *name, int fallback)
{
    const char *value = getenv(name);
    return (value && *value) ? atoi(value) : fallback;
}

/* Expected texture coordinates of the panel corners (top-left, bottom-left,
   top-right, bottom-right), written out from pf-framehost's pixel table:
   "Left Side Up" (90) shows landscape (W-1-y, x) at panel (x, y), so the panel's
   top-left shows the landscape top-right (1,0) and the landscape top-left
   (0,0) is at the panel's bottom-left; "Right Side Up" (270) shows (y, H-1-x),
   so the landscape top-left is at the panel's top-right. */
static const float *expected_uv(int rotation)
{
    static const float uv90[8] = { 1, 0, 0, 0, 1, 1, 0, 1 };
    static const float uv180[8] = { 1, 1, 1, 0, 0, 1, 0, 0 };
    static const float uv270[8] = { 0, 1, 1, 1, 0, 0, 1, 0 };
    switch (rotation) {
    case 90:
        return uv90;
    case 180:
        return uv180;
    case 270:
        return uv270;
    default:
        return NULL;
    }
}

static const char *corner_name(int index)
{
    static const char *const names[4] = { "top-left", "bottom-left", "top-right", "bottom-right" };
    return (index >= 0 && index < 4) ? names[index] : "nowhere";
}

int main(void)
{
    const int expect_rotation = env_int("EXPECT_ROTATION", 0);
    const int expect_panel_prop = env_int("EXPECT_PANEL_PROP", 0);
    const int atomic = env_int("FAKE_KMS_ATOMIC", 1);
    const int fence_sync = env_int("FAKE_EGL_FENCE_SYNC", 1);
    const int native_fence = env_int("FAKE_EGL_NATIVE_FENCE", 1);
    const int flip_ms = env_int("FAKE_KMS_FLIP_MS", 0);
    const int expect_window_fail = env_int("EXPECT_WINDOW_FAIL", 0);
    const int expect_vulkan_window = env_int("EXPECT_VULKAN_WINDOW", 0);
    const int expect_makecurrent_fail = env_int("EXPECT_MAKECURRENT_FAIL", 0);
    const int expect_app_rotation = (expect_panel_prop - expect_rotation + 360) % 360;
    const int swaps_axes = expect_rotation == 90 || expect_rotation == 270;
    const int lw = swaps_axes ? FAKE_KMS_PANEL_H : FAKE_KMS_PANEL_W;
    const int lh = swaps_axes ? FAKE_KMS_PANEL_W : FAKE_KMS_PANEL_H;
    const int frames = 8;
    void *fake;
    FakeKmsGetReportFn get_report;
    const FakeKmsReport *report;
    SDL_DisplayID display;
    const SDL_DisplayMode *desktop;
    SDL_PropertiesID display_props;
    SDL_Window *window;
    SDL_GLContext context;
    PFNGLCLEARPROC glClearFn;
    PFNGLCLEARCOLORPROC glClearColorFn;
    PFNGLVIEWPORTPROC glViewportFn;
    int i, w = 0, h = 0, app_surface, alive = 0, locked = 0;
    long long panel_prop, present_prop, window_present, window_app;

    fake = dlopen("libdrm.so.2", RTLD_NOW | RTLD_LOCAL);
    get_report = fake ? (FakeKmsGetReportFn)dlsym(fake, "fake_kms_get_report") : NULL;
    if (!get_report) {
        printf("FAIL: libdrm.so.2 on the library path is not the fake display stack\n");
        return 2;
    }
    report = get_report();

    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "kmsdrm");
    SDL_SetHint(SDL_HINT_KMSDRM_DEVICE_INDEX, "0");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("FAIL: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    CHECK(strcmp(SDL_GetCurrentVideoDriver(), "kmsdrm") == 0, "video driver is %s", SDL_GetCurrentVideoDriver());

    display = SDL_GetPrimaryDisplay();
    desktop = SDL_GetDesktopDisplayMode(display);
    CHECK(desktop && desktop->w == lw && desktop->h == lh, "desktop mode %dx%d, expected %dx%d",
          desktop ? desktop->w : -1, desktop ? desktop->h : -1, lw, lh);
    display_props = SDL_GetDisplayProperties(display);
    panel_prop = (long long)SDL_GetNumberProperty(display_props, SDL_PROP_DISPLAY_KMSDRM_PANEL_ORIENTATION_NUMBER, -1);
    present_prop = (long long)SDL_GetNumberProperty(display_props, "SDL.display.KMSDRM.pocketforge.present_rotation", -1);
    CHECK(panel_prop == expect_panel_prop, "display panel_orientation property %lld (the true panel orientation), expected %d",
          panel_prop, expect_panel_prop);
    CHECK(present_prop == expect_rotation, "display present_rotation property %lld, expected %d", present_prop, expect_rotation);

    if (expect_vulkan_window) {
        window = SDL_CreateWindow("kmsdrm-rotation-vulkan", lw, lh, SDL_WINDOW_VULKAN);
        CHECK(window != NULL, "Vulkan window created (%s)", window ? "ok" : SDL_GetError());
        CHECK(report->vulkan_enumerations > 0, "SDL loaded Vulkan from the fake libvulkan.so.1 (%d enumerations)",
              report->vulkan_enumerations);
        if (window) {
            SDL_PropertiesID props = SDL_GetWindowProperties(window);
            window_present = (long long)SDL_GetNumberProperty(props, "SDL.window.KMSDRM.pocketforge.present_rotation", -1);
            window_app = (long long)SDL_GetNumberProperty(props, "SDL.window.KMSDRM.pocketforge.app_rotation", -1);
            CHECK(window_present == 0, "Vulkan window: SDL applies no rotation (window present_rotation %lld)", window_present);
            CHECK(window_app == expect_panel_prop,
                  "Vulkan window: the application owns the panel's %d degrees (window app_rotation %lld)",
                  expect_panel_prop, window_app);
            SDL_DestroyWindow(window);
        }
        CHECK(report->surfaces_created == 0 && report->contexts_created == 0,
              "Vulkan window: no GBM surface (%d) or EGL context (%d) was made", report->surfaces_created,
              report->contexts_created);
        CHECK(report->errors == 0, "the fake display stack saw %d contract violations%s%s", report->errors,
              report->errors ? "; first: " : "", report->first_error);
        SDL_Quit();
        printf("RESULT: %s\n", failures ? "FAIL" : "PASS");
        return failures ? 1 : 0;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    window = SDL_CreateWindow("kmsdrm-rotation", desktop ? desktop->w : lw, desktop ? desktop->h : lh,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
    if (expect_window_fail) {
        printf("SDL_CreateWindow: %s (%s)\n", window ? "created" : "failed", window ? "" : SDL_GetError());
        CHECK(report->omitted_proc_requests >= 1,
              "the rotate pass asked for the missing mid-table entry point (%d requests)", report->omitted_proc_requests);
        CHECK(!window, "a GL entry point missing mid-table fails GL window creation instead of presenting unrotated");
        CHECK(report->draws == 0 && report->images_created == 0, "no rotate pass ran (%d draws, %d images)",
              report->draws, report->images_created);
        if (window) {
            SDL_DestroyWindow(window);
        }
        for (i = 0; i < report->surfaces_created; ++i) {
            alive += report->surfaces[i].alive;
            locked += report->surfaces[i].locked_now;
        }
        CHECK(alive == 0 && locked == 0, "after the failure: no GBM surface alive (%d) and no buffer locked (%d)",
              alive, locked);
        CHECK(report->contexts_alive == 0 && report->syncs_alive == 0,
              "after the failure: no EGL context (%d) or sync (%d) alive", report->contexts_alive, report->syncs_alive);
        CHECK(report->errors == 0, "the fake display stack saw %d contract violations%s%s", report->errors,
              report->errors ? "; first: " : "", report->first_error);
        SDL_Quit();
        printf("RESULT: %s\n", failures ? "FAIL" : "PASS");
        return failures ? 1 : 0;
    }
    if (!window) {
        printf("FAIL: SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    {
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        window_present = (long long)SDL_GetNumberProperty(props, "SDL.window.KMSDRM.pocketforge.present_rotation", -1);
        window_app = (long long)SDL_GetNumberProperty(props, "SDL.window.KMSDRM.pocketforge.app_rotation", -1);
        CHECK(window_present == expect_rotation, "GL window: window present_rotation %lld, expected %d", window_present,
              expect_rotation);
        CHECK(window_app == expect_app_rotation, "GL window: window app_rotation %lld, expected %d", window_app,
              expect_app_rotation);
    }
    context = SDL_GL_CreateContext(window);
    if (!context || !SDL_GL_MakeCurrent(window, context)) {
        printf("FAIL: GL context: %s\n", SDL_GetError());
        return 1;
    }
    glClearFn = (PFNGLCLEARPROC)SDL_GL_GetProcAddress("glClear");
    glClearColorFn = (PFNGLCLEARCOLORPROC)SDL_GL_GetProcAddress("glClearColor");
    glViewportFn = (PFNGLVIEWPORTPROC)SDL_GL_GetProcAddress("glViewport");
    if (!glClearFn || !glClearColorFn || !glViewportFn) {
        printf("FAIL: GL entry points\n");
        return 1;
    }

    if (expect_makecurrent_fail) {
        bool ok;
        int egl_alive, contexts_besides_app;
        glViewportFn(0, 0, lw, lh);
        glClearFn(GL_COLOR_BUFFER_BIT);
        ok = SDL_GL_SwapWindow(window);
        printf("frame 1: SDL_GL_SwapWindow %s (%s)\n", ok ? "succeeded" : "failed", ok ? "" : SDL_GetError());
        CHECK(report->injected_makecurrent_failures == 1,
              "the final eglMakeCurrent of the frame-1 surface rebuild failed (%d injected failures)",
              report->injected_makecurrent_failures);
        CHECK(!ok, "the failed surface rebuild fails the swap");
        for (i = 0; i < report->surfaces_created; ++i) {
            alive += report->surfaces[i].alive;
            locked += report->surfaces[i].locked_now;
        }
        egl_alive = report->egl_surfaces_alive;
        contexts_besides_app = report->contexts_alive - 1;
        CHECK(alive == 0 && locked == 0 && egl_alive == 0 && contexts_besides_app == 0 && report->images_alive == 0 &&
                  report->syncs_alive == 0,
              "after the failed surface rebuild: nothing leaked (%d GBM surfaces, %d locked buffers, %d EGL surfaces, "
              "%d contexts besides the application's, %d images, %d syncs alive)",
              alive, locked, egl_alive, contexts_besides_app, report->images_alive, report->syncs_alive);
        CHECK(report->errors == 0, "the fake display stack saw %d contract violations%s%s", report->errors,
              report->errors ? "; first: " : "", report->first_error);
        SDL_GL_DestroyContext(context);
        SDL_DestroyWindow(window);
        alive = locked = 0;
        for (i = 0; i < report->surfaces_created; ++i) {
            alive += report->surfaces[i].alive;
            locked += report->surfaces[i].locked_now;
        }
        CHECK(alive == 0 && locked == 0 && report->egl_surfaces_alive == 0 && report->contexts_alive == 0,
              "teardown: no GBM surface (%d), locked buffer (%d), EGL surface (%d) or context (%d) alive", alive, locked,
              report->egl_surfaces_alive, report->contexts_alive);
        CHECK(report->errors == 0, "teardown: %d contract violations%s%s", report->errors,
              report->errors ? "; first: " : "", report->first_error);
        SDL_Quit();
        printf("RESULT: %s\n", failures ? "FAIL" : "PASS");
        return failures ? 1 : 0;
    }

    for (i = 1; i <= frames; ++i) {
        bool ok;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        glViewportFn(0, 0, w, h);
        glClearColorFn(0.1f * (float)i, 0.2f, 0.3f, 1.0f);
        glClearFn(GL_COLOR_BUFFER_BIT);
        ok = SDL_GL_SwapWindow(window);
        /* Frame 1 follows SDL's fullscreen switch, which marks the surfaces for
           recreation. Upstream's atomic paths then commit an empty request and
           return false for that one swap; that is not what this test measures. */
        if (i >= 2) {
            CHECK(ok, "frame %d: SDL_GL_SwapWindow %s", i, ok ? "succeeded" : SDL_GetError());
        }
        CHECK(report->current_ctx == (void *)context,
              "frame %d: the application's context is current again after the swap", i);
    }

    SDL_GetWindowSizeInPixels(window, &w, &h);
    CHECK(w == lw && h == lh, "SDL_GetWindowSizeInPixels %dx%d, expected %dx%d", w, h, lw, lh);
    SDL_GetWindowSize(window, &w, &h);
    CHECK(w == lw && h == lh, "SDL_GetWindowSize %dx%d, expected %dx%d", w, h, lw, lh);

    app_surface = report->current_draw_surface;
    CHECK(app_surface >= 0 && report->surfaces[app_surface].w == lw && report->surfaces[app_surface].h == lh,
          "the application renders into a %dx%d surface (expected %dx%d)",
          app_surface >= 0 ? report->surfaces[app_surface].w : -1,
          app_surface >= 0 ? report->surfaces[app_surface].h : -1, lw, lh);
    CHECK(report->scanouts > 0 && report->scanout_w == FAKE_KMS_PANEL_W && report->scanout_h == FAKE_KMS_PANEL_H,
          "the panel scans out a %dx%d buffer (%d scanouts)", report->scanout_w, report->scanout_h, report->scanouts);
    if (atomic && flip_ms > 0) {
        CHECK(report->busy_commits == 0,
              "flips take %d ms: no nonblocking commit reached a pending flip (%d -EBUSY)", flip_ms,
              report->busy_commits);
        if (native_fence) {
            CHECK(report->flip_fences >= frames - 2, "the fenced path asked for an OUT_FENCE per flip (%d)",
                  report->flip_fences);
        }
    }
    if (atomic) {
        CHECK(report->plane_src_w == FAKE_KMS_PANEL_W && report->plane_src_h == FAKE_KMS_PANEL_H &&
                  report->plane_crtc_w == FAKE_KMS_PANEL_W && report->plane_crtc_h == FAKE_KMS_PANEL_H,
              "atomic plane source %dx%d and destination %dx%d are panel-native",
              report->plane_src_w, report->plane_src_h, report->plane_crtc_w, report->plane_crtc_h);
    }
    for (i = 0; i < report->surfaces_created; ++i) {
        alive += report->surfaces[i].alive;
    }

    if (expect_rotation != 0) {
        const FakeKmsDraw *draw = &report->last_draw;
        const int present = report->scanout_surface;
        const float *uv = expected_uv(expect_rotation);
        int j, uv_ok = 1, tl_corner = -1;

        CHECK(alive == 2, "two GBM surfaces are alive: the application's and the panel-native present surface (%d)", alive);
        CHECK(present >= 0 && present != app_surface && report->surfaces[present].w == FAKE_KMS_PANEL_W &&
                  report->surfaces[present].h == FAKE_KMS_PANEL_H &&
                  (report->surfaces[present].flags & GBM_BO_USE_SCANOUT),
              "the scanned-out buffer comes from the 720x1280 scanout surface, not the application's");
        CHECK(report->contexts_alive == 2, "application context plus one rotate context (%d contexts)", report->contexts_alive);
        CHECK(report->draws >= frames - 2, "%d rotate passes for %d frames", report->draws, frames);
        CHECK(draw->ctx != (void *)context, "the rotate pass ran in its own context, not the application's");
        CHECK(draw->target_surface == present, "the rotate pass drew into the present surface");
        CHECK(draw->source_surface == app_surface && draw->source_w == lw && draw->source_h == lh,
              "the rotate pass sampled the application's %dx%d buffer", draw->source_w, draw->source_h);
        CHECK(draw->source_frame == report->surfaces[app_surface].swaps,
              "the last pass sampled the frame just swapped (frame %d of %d)", draw->source_frame,
              report->surfaces[app_surface].swaps);
        CHECK(draw->viewport[0] == 0 && draw->viewport[1] == 0 && draw->viewport[2] == FAKE_KMS_PANEL_W &&
                  draw->viewport[3] == FAKE_KMS_PANEL_H,
              "the pass covers the whole panel (viewport %d,%d %dx%d)",
              draw->viewport[0], draw->viewport[1], draw->viewport[2], draw->viewport[3]);
        for (j = 0; j < 8; ++j) {
            if (!uv || draw->uv[j] != uv[j]) {
                uv_ok = 0;
            }
        }
        for (j = 0; j < 4; ++j) {
            if (draw->uv[2 * j] == 0.0f && draw->uv[2 * j + 1] == 0.0f) {
                tl_corner = j;
            }
        }
        CHECK(uv_ok, "rotation %d: panel corners sample landscape (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f); "
                     "the landscape top-left lands at the panel's %s corner",
              expect_rotation, draw->uv[0], draw->uv[1], draw->uv[2], draw->uv[3], draw->uv[4], draw->uv[5],
              draw->uv[6], draw->uv[7], corner_name(tl_corner));
        CHECK(draw->pos[0] == -1.0f && draw->pos[1] == 1.0f && draw->pos[6] == 1.0f && draw->pos[7] == -1.0f,
              "the pass strip spans clip space from the panel's top-left to its bottom-right");
        CHECK(report->surfaces[app_surface].locked_max <= 2,
              "at most two application buffers are held at once (%d)", report->surfaces[app_surface].locked_max);
        if (fence_sync) {
            CHECK(report->cross_context_waits >= report->draws,
                  "each pass waits on the application's fence on the GPU (%d waits, %d passes)",
                  report->cross_context_waits, report->draws);
        }
    } else {
        CHECK(alive == 1, "negative control: one GBM surface is alive (%d)", alive);
        CHECK(report->scanout_surface == app_surface, "negative control: the application's own surface is scanned out");
        CHECK(report->contexts_alive == 1, "negative control: no rotate context (%d contexts)", report->contexts_alive);
        CHECK(report->images_created == 0, "negative control: no EGLImages (%d)", report->images_created);
        CHECK(report->draws == 0, "negative control: no rotate pass (%d)", report->draws);
    }
    CHECK(report->errors == 0, "the fake display stack saw %d contract violations%s%s", report->errors,
          report->errors ? "; first: " : "", report->first_error);

    SDL_GL_DestroyContext(context);
    SDL_DestroyWindow(window);

    alive = 0;
    for (i = 0; i < report->surfaces_created; ++i) {
        alive += report->surfaces[i].alive;
        locked += report->surfaces[i].locked_now;
    }
    CHECK(alive == 0 && locked == 0, "teardown: no GBM surface alive (%d) and no buffer locked (%d)", alive, locked);
    CHECK(report->egl_surfaces_alive == 0, "teardown: no EGL surface alive (%d)", report->egl_surfaces_alive);
    CHECK(report->contexts_alive == 0, "teardown: no EGL context alive (%d)", report->contexts_alive);
    CHECK(report->images_alive == 0, "teardown: no EGLImage alive (%d)", report->images_alive);
    CHECK(report->errors == 0, "teardown: %d contract violations%s%s", report->errors,
          report->errors ? "; first: " : "", report->first_error);

    SDL_Quit();
    printf("RESULT: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
