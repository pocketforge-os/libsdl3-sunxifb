/* Hermetic full-stack cursor bitmap/hotspot contract for renderer pre-rotation. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "fake-kms.h"
#include "SDL_kmsdrmprerotate.h"

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

int main(void)
{
    enum { CURSOR_W = 4, CURSOR_H = 3, HOT_X = 1, HOT_Y = 0 };
    const int panel_rotation = getenv("EXPECT_ROTATION") ? atoi(getenv("EXPECT_ROTATION")) : 0;
    const int enabled = getenv("EXPECT_PREROTATE") && atoi(getenv("EXPECT_PREROTATE"));
    const int rotation = enabled ? panel_rotation : 0;
    unsigned char expected[FAKE_KMS_CURSOR_BYTES] = { 0 };
    void *fake;
    FakeKmsGetReportFn get_report;
    const FakeKmsReport *report;
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Surface *surface = NULL;
    SDL_Cursor *cursor = NULL;
    int logical_w, logical_h, expected_hot_x, expected_hot_y;
    int writes_before_renderer;
    int expected_w = KMSDRM_RotationSwapsAxes(rotation) ? CURSOR_H : CURSOR_W;
    int expected_h = KMSDRM_RotationSwapsAxes(rotation) ? CURSOR_W : CURSOR_H;
    int x, y;

    fake = dlopen("libdrm.so.2", RTLD_NOW | RTLD_LOCAL);
    get_report = fake ? (FakeKmsGetReportFn)dlsym(fake, "fake_kms_get_report") : NULL;
    if (!get_report) {
        printf("FAIL: libdrm.so.2 on the library path is not the fake display stack\n");
        return 2;
    }
    report = get_report();

    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "kmsdrm");
    SDL_SetHint(SDL_HINT_KMSDRM_DEVICE_INDEX, "0");
    SDL_SetHint(SDL_HINT_KMSDRM_RENDERER_PREROTATION, enabled ? "1" : "0");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("FAIL: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    KMSDRM_RotationLogicalSize(panel_rotation, FAKE_KMS_PANEL_W, FAKE_KMS_PANEL_H,
                               &logical_w, &logical_h);
    window = SDL_CreateWindow("kmsdrm-cursor-prerotate", logical_w, logical_h, 0);
    CHECK(window != NULL, "create %dx%d logical window for rotation %d%s%s",
          logical_w, logical_h, panel_rotation, window ? "" : ": ", window ? "" : SDL_GetError());
    if (!window) {
        goto out;
    }
    writes_before_renderer = report->cursor_write_calls;
    renderer = SDL_CreateRenderer(window, "opengles2");
    CHECK(renderer != NULL, "create GLES2 renderer%s%s",
          renderer ? "" : ": ", renderer ? "" : SDL_GetError());
    if (!renderer) {
        goto out;
    }
    CHECK((int)SDL_GetBooleanProperty(SDL_GetWindowProperties(window),
                                      KMSDRM_PREROTATION_ACTIVE_PROPERTY, false) ==
              (enabled && panel_rotation != 0),
          "renderer pre-rotation active state matches enabled=%d rotation=%d",
          enabled, panel_rotation);
    if (enabled && panel_rotation != 0) {
        CHECK(report->cursor_write_calls > writes_before_renderer,
              "renderer activation re-uploads the existing default cursor (%d -> %d writes)",
              writes_before_renderer, report->cursor_write_calls);
    }

    surface = SDL_CreateSurface(CURSOR_W, CURSOR_H, SDL_PIXELFORMAT_ARGB8888);
    CHECK(surface != NULL, "create asymmetric %dx%d cursor surface", CURSOR_W, CURSOR_H);
    if (!surface) {
        goto out;
    }
    SDL_memset(surface->pixels, 0, (size_t)surface->pitch * surface->h);
    for (y = 0; y < CURSOR_H; ++y) {
        for (x = 0; x < CURSOR_W; ++x) {
            Uint32 pixel = SDL_MapSurfaceRGBA(surface,
                                              (Uint8)(0x20 + x * 0x20),
                                              (Uint8)(0x30 + y * 0x30),
                                              (Uint8)(1 + y * CURSOR_W + x),
                                              0xff);
            int px, py;

            SDL_memcpy((Uint8 *)surface->pixels + y * surface->pitch + x * 4,
                       &pixel, sizeof(pixel));
            KMSDRM_RotationLogicalToPhysical(rotation, CURSOR_W, CURSOR_H,
                                             x, y, &px, &py);
            SDL_memcpy(expected + py * FAKE_KMS_CURSOR_W * 4 + px * 4,
                       &pixel, sizeof(pixel));
        }
    }

    cursor = SDL_CreateColorCursor(surface, HOT_X, HOT_Y);
    CHECK(cursor != NULL, "create cursor with nonzero hotspot (%d,%d)%s%s",
          HOT_X, HOT_Y, cursor ? "" : ": ", cursor ? "" : SDL_GetError());
    if (!cursor) {
        goto out;
    }
    CHECK(SDL_SetCursor(cursor), "show asymmetric cursor");

    KMSDRM_RotationLogicalToPhysical(rotation, CURSOR_W, CURSOR_H,
                                     HOT_X, HOT_Y, &expected_hot_x, &expected_hot_y);
    CHECK(report->cursor_bo_w == FAKE_KMS_CURSOR_W &&
              report->cursor_bo_h == FAKE_KMS_CURSOR_H &&
              report->cursor_bo_stride == FAKE_KMS_CURSOR_W * 4 &&
              report->cursor_bo_size == FAKE_KMS_CURSOR_BYTES,
          "cursor BO is the expected %dx%d stride-%d allocation",
          report->cursor_bo_w, report->cursor_bo_h, report->cursor_bo_stride);
    CHECK(report->cursor_write_calls > 0 &&
              memcmp(report->cursor_bo, expected, sizeof(expected)) == 0,
          "%d-degree cursor BO is a pixel-exact %dx%d -> %dx%d transform",
          rotation, CURSOR_W, CURSOR_H, expected_w, expected_h);
    CHECK(report->cursor_hot_x == expected_hot_x &&
              report->cursor_hot_y == expected_hot_y,
          "%d-degree cursor hotspot is (%d,%d), expected (%d,%d)",
          rotation, report->cursor_hot_x, report->cursor_hot_y,
          expected_hot_x, expected_hot_y);
    CHECK(report->errors == 0, "fake display stack saw %d contract violations%s%s",
          report->errors, report->errors ? "; first: " : "", report->first_error);

out:
    SDL_DestroyCursor(cursor);
    SDL_DestroySurface(surface);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    printf("RESULT: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
