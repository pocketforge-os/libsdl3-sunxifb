/*
 * tsp-mc9m.41.924.16.13.3.3: exercise the SDL_Renderer pre-rotation
 * handshake through SDL's real KMSDRM and GLES2 renderer implementations.
 * The fake display stack records GL geometry, surface ownership and scanout.
 *
 * EXPECT_PREROTATE=1 is the positive case. EXPECT_PREROTATE=0 is the explicit
 * opt-out control: the same renderer uses the established rotate pass. The
 * harness selects the hint through the environment, including leaving it
 * absent to exercise the default.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "fake-kms.h"
#include "SDL_kmsdrmprerotate.h"

static int failures;

typedef struct ResizeWatch
{
    int physical_resizes;
} ResizeWatch;

static bool SDLCALL watch_resizes(void *userdata, SDL_Event *event)
{
    ResizeWatch *watch = (ResizeWatch *)userdata;

    if (event->type == SDL_EVENT_WINDOW_RESIZED &&
        event->window.data1 == FAKE_KMS_PANEL_W &&
        event->window.data2 == FAKE_KMS_PANEL_H) {
        ++watch->physical_resizes;
    }
    return true;
}

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

static float absf(float value)
{
    return value < 0.0f ? -value : value;
}

int main(void)
{
    const int enabled = getenv("EXPECT_PREROTATE") && atoi(getenv("EXPECT_PREROTATE"));
    const int rotation = getenv("EXPECT_ROTATION") ? atoi(getenv("EXPECT_ROTATION")) : 90;
    void *fake;
    FakeKmsGetReportFn get_report;
    const FakeKmsReport *report;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *target;
    SDL_Surface *readback;
    SDL_Rect viewport = { 100, 50, 300, 200 };
    SDL_Rect clip = { 7, 11, 40, 30 };
    SDL_FRect fill = { 9.0f, 13.0f, 20.0f, 15.0f };
    SDL_Rect read_rect = { 110, 60, 40, 30 };
    const int cursor_logical[4][2] = {
        { 0, 0 }, { 1279, 0 }, { 0, 719 }, { 1279, 719 }
    };
    const int cursor_physical_90[4][2] = {
        { 0, 1279 }, { 0, 0 }, { 719, 1279 }, { 719, 0 }
    };
    const int cursor_physical_270[4][2] = {
        { 719, 0 }, { 719, 1279 }, { 0, 0 }, { 0, 1279 }
    };
    ResizeWatch resize_watch = { 0 };
    int i, alive = 0, w = 0, h = 0;
    long long candidate, active;

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

    SDL_AddEventWatch(watch_resizes, &resize_watch);

    window = SDL_CreateWindow("kmsdrm-renderer-prerotate", 1280, 720, 0);
    SDL_RemoveEventWatch(watch_resizes, &resize_watch);
    if (!window) {
        printf("FAIL: SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    candidate = (long long)SDL_GetNumberProperty(
        SDL_GetWindowProperties(window),
        "SDL.window.KMSDRM.pocketforge.renderer_prerotation", 0);
    CHECK(candidate == (enabled ? rotation : 0), "candidate rotation is %lld (expected %d)",
          candidate, enabled ? rotation : 0);
    CHECK(resize_watch.physical_resizes == 0,
          "candidate publishes no panel-native WINDOW_RESIZED events (%d)",
          resize_watch.physical_resizes);
    SDL_GetWindowSize(window, &w, &h);
    CHECK(w == 1280 && h == 720, "candidate public window size is logical %dx%d", w, h);
    SDL_GetWindowSizeInPixels(window, &w, &h);
    CHECK(w == (enabled ? 720 : 1280) && h == (enabled ? 1280 : 720),
          "candidate pixel/drawable size is %dx%d (expected %dx%d)", w, h,
          enabled ? 720 : 1280, enabled ? 1280 : 720);

    renderer = SDL_CreateRenderer(window, "opengles2");
    if (!renderer) {
        printf("FAIL: SDL_CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }
    active = (long long)SDL_GetBooleanProperty(
        SDL_GetWindowProperties(window),
        "SDL.window.KMSDRM.pocketforge.renderer_prerotation_active", false);
    CHECK(active == enabled, "GLES2 renderer handshake is %lld (expected %d)", active, enabled);

    target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                               SDL_TEXTUREACCESS_TARGET, 64, 32);
    CHECK(target != NULL, "create 64x32 offscreen render target%s%s",
          target ? "" : ": ", target ? "" : SDL_GetError());
    CHECK(target && SDL_SetRenderTarget(renderer, target), "select offscreen render target");
    CHECK(SDL_SetRenderViewport(renderer, NULL), "use complete offscreen viewport");
    CHECK(SDL_SetRenderClipRect(renderer, NULL), "disable offscreen clip");
    CHECK(SDL_SetRenderDrawColor(renderer, 0x77, 0x22, 0x44, 0xff), "set offscreen draw colour");
    CHECK(SDL_RenderFillRect(renderer, NULL), "queue offscreen primitive");
    CHECK(SDL_FlushRenderer(renderer), "flush offscreen primitive");
    CHECK(report->renderer_texture_target_draws == 1 &&
              report->renderer_texture_target_viewport[0] == 0 &&
              report->renderer_texture_target_viewport[1] == 0 &&
              report->renderer_texture_target_viewport[2] == 64 &&
              report->renderer_texture_target_viewport[3] == 32,
          "offscreen target keeps its unrotated 64x32 viewport");
    CHECK(absf(report->renderer_texture_target_projection[0] - (2.0f / 64.0f)) < 0.000001f &&
              absf(report->renderer_texture_target_projection[5] - (2.0f / 32.0f)) < 0.000001f,
          "offscreen target keeps SDL's ordinary projection");
    CHECK(SDL_SetRenderTarget(renderer, NULL), "restore window render target");

    SDL_GetWindowSize(window, &w, &h);
    CHECK(w == 1280 && h == 720, "logical window remains %dx%d", w, h);
    SDL_GetWindowSizeInPixels(window, &w, &h);
    CHECK(w == (enabled ? 720 : 1280) && h == (enabled ? 1280 : 720),
          "pixel/drawable size remains %dx%d", w, h);
    SDL_GetRenderOutputSize(renderer, &w, &h);
    CHECK(w == 1280 && h == 720, "SDL_Renderer output remains logical %dx%d", w, h);

    for (i = 0; i < 4; ++i) {
        const int (*expected)[2] = rotation == 270 ? cursor_physical_270 : cursor_physical_90;
        const int expected_x = enabled ? expected[i][0] : cursor_logical[i][0];
        const int expected_y = enabled ? expected[i][1] : cursor_logical[i][1];
        SDL_WarpMouseInWindow(window, (float)cursor_logical[i][0],
                              (float)cursor_logical[i][1]);
        CHECK(report->cursor_x == expected_x && report->cursor_y == expected_y,
              "logical cursor corner (%d,%d) maps to physical (%d,%d), expected (%d,%d)",
              cursor_logical[i][0], cursor_logical[i][1], report->cursor_x, report->cursor_y,
              expected_x, expected_y);
    }
    CHECK(SDL_SetRenderViewport(renderer, &viewport), "set asymmetric logical viewport");
    CHECK(SDL_SetRenderClipRect(renderer, &clip), "set asymmetric logical clip rectangle");
    CHECK(SDL_SetRenderDrawColor(renderer, 0x11, 0x55, 0xaa, 0xff), "set renderer draw colour");
    for (i = 0; i < 3; ++i) {
        CHECK(SDL_RenderFillRect(renderer, &fill), "queue asymmetric renderer primitive for frame %d", i + 1);
        CHECK(SDL_FlushRenderer(renderer), "flush renderer primitive for frame %d", i + 1);
        if (i == 2) {
            readback = SDL_RenderReadPixels(renderer, &read_rect);
            CHECK(readback && readback->w == read_rect.w && readback->h == read_rect.h,
                  "SDL_RenderReadPixels returns logical %dx%d%s%s", read_rect.w, read_rect.h,
                  readback ? "" : ": ", readback ? "" : SDL_GetError());
            SDL_DestroySurface(readback);
        }
        CHECK(SDL_RenderPresent(renderer), "present SDL_Renderer frame %d", i + 1);
    }

    for (i = 0; i < report->surfaces_created; ++i) {
        alive += report->surfaces[i].alive;
    }
    CHECK(report->renderer_draws > 0, "GLES2 SDL_Renderer issued %d primitive draw(s)", report->renderer_draws);

    if (enabled) {
        const KMSDRM_PreRotationRect logical_viewport = {
            viewport.x, viewport.y, viewport.w, viewport.h
        };
        const KMSDRM_PreRotationRect logical_clip = {
            clip.x, clip.y, clip.w, clip.h
        };
        const KMSDRM_PreRotationRect logical_read = {
            read_rect.x, read_rect.y, read_rect.w, read_rect.h
        };
        KMSDRM_PreRotationRect physical_viewport, physical_clip, physical_read;
        float expected_projection[16];
        const float *p = report->renderer_projection;

        KMSDRM_PreRotationLogicalRectToPhysical(rotation, 1280, 720,
                                                &logical_viewport, &physical_viewport);
        KMSDRM_PreRotationLogicalRectToPhysical(rotation, viewport.w, viewport.h,
                                                &logical_clip, &physical_clip);
        physical_clip.x += physical_viewport.x;
        physical_clip.y += physical_viewport.y;
        KMSDRM_PreRotationLogicalRectToPhysical(rotation, 1280, 720,
                                                &logical_read, &physical_read);
        KMSDRM_PreRotationProjection(rotation, viewport.w, viewport.h, expected_projection);
        CHECK(alive == 1, "pre-rotation uses one live GBM surface (%d)", alive);
        CHECK(report->contexts_alive == 1 && report->images_created == 0 && report->draws == 0,
              "pre-rotation has one context, no imported image and no rotate pass (%d, %d, %d)",
              report->contexts_alive, report->images_created, report->draws);
        CHECK(report->renderer_target_surface == report->scanout_surface,
              "SDL_Renderer draws directly into the scanned-out surface");
        CHECK(report->scanout_w == FAKE_KMS_PANEL_W && report->scanout_h == FAKE_KMS_PANEL_H,
              "direct scanout is panel-native %dx%d", report->scanout_w, report->scanout_h);
        CHECK(report->renderer_viewport[0] == physical_viewport.x &&
                  report->renderer_viewport[1] == FAKE_KMS_PANEL_H - physical_viewport.y - physical_viewport.h &&
                  report->renderer_viewport[2] == physical_viewport.w &&
                  report->renderer_viewport[3] == physical_viewport.h,
              "logical viewport maps to GL viewport {%d,%d,%d,%d}",
              report->renderer_viewport[0], report->renderer_viewport[1],
              report->renderer_viewport[2], report->renderer_viewport[3]);
        CHECK(report->renderer_scissor[0] == physical_clip.x &&
                  report->renderer_scissor[1] == FAKE_KMS_PANEL_H - physical_clip.y - physical_clip.h &&
                  report->renderer_scissor[2] == physical_clip.w &&
                  report->renderer_scissor[3] == physical_clip.h,
              "logical clip maps to GL scissor {%d,%d,%d,%d}",
              report->renderer_scissor[0], report->renderer_scissor[1],
              report->renderer_scissor[2], report->renderer_scissor[3]);
        for (i = 0; i < 16; ++i) {
            if (absf(p[i] - expected_projection[i]) >= 0.000001f) {
                break;
            }
        }
        CHECK(i == 16, "GLES2 received the %d-degree projection matrix", rotation);
        CHECK(report->readpixels_calls == 1 && report->readpixels_rect[0] == physical_read.x &&
                  report->readpixels_rect[1] == FAKE_KMS_PANEL_H - physical_read.y - physical_read.h &&
                  report->readpixels_rect[2] == physical_read.w &&
                  report->readpixels_rect[3] == physical_read.h,
              "logical readback maps to physical GL rect {%d,%d,%d,%d}",
              report->readpixels_rect[0], report->readpixels_rect[1],
              report->readpixels_rect[2], report->readpixels_rect[3]);
    } else {
        CHECK(alive == 2, "negative control keeps logical and panel-native surfaces (%d)", alive);
        CHECK(report->contexts_alive == 2 && report->images_created > 0 && report->draws > 0,
              "negative control keeps the imported-image rotate pass (%d contexts, %d images, %d passes)",
              report->contexts_alive, report->images_created, report->draws);
        CHECK(report->renderer_target_surface != report->scanout_surface,
              "negative control renders logical surface before the present pass");
        CHECK(report->renderer_viewport[0] == 100 && report->renderer_viewport[1] == 470 &&
                  report->renderer_viewport[2] == 300 && report->renderer_viewport[3] == 200,
              "negative control keeps logical GL viewport {%d,%d,%d,%d}",
              report->renderer_viewport[0], report->renderer_viewport[1],
              report->renderer_viewport[2], report->renderer_viewport[3]);
        CHECK(report->readpixels_calls == 1 && report->readpixels_rect[0] == 110 &&
                  report->readpixels_rect[1] == 630 && report->readpixels_rect[2] == 40 &&
                  report->readpixels_rect[3] == 30,
              "negative control reads logical GL rect {%d,%d,%d,%d}",
              report->readpixels_rect[0], report->readpixels_rect[1],
              report->readpixels_rect[2], report->readpixels_rect[3]);
    }
    CHECK(report->errors == 0, "fake display stack saw %d contract violations%s%s", report->errors,
          report->errors ? "; first: " : "", report->first_error);

    SDL_DestroyTexture(target);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    printf("RESULT: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
