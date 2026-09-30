/* Hermetic contract for KMSDRM SDL_Renderer pre-rotation. */
#include <math.h>
#include <stdio.h>
#include <string.h>

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

static int closef(float a, float b)
{
    return fabsf(a - b) < 0.0001f;
}

static void test_rects(void)
{
    static const struct {
        int rotation;
        KMSDRM_PreRotationRect expected;
    } cases[] = {
        { 0, { 100, 50, 300, 200 } },
        { 90, { 50, 880, 200, 300 } },
        { 180, { 880, 470, 300, 200 } },
        { 270, { 470, 100, 200, 300 } },
    };
    const KMSDRM_PreRotationRect logical = { 100, 50, 300, 200 };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        KMSDRM_PreRotationRect physical;
        KMSDRM_PreRotationLogicalRectToPhysical(cases[i].rotation, 1280, 720,
                                                &logical, &physical);
        CHECK(memcmp(&physical, &cases[i].expected, sizeof(physical)) == 0,
              "rotation %d maps partial viewport to {%d,%d,%d,%d}",
              cases[i].rotation, physical.x, physical.y, physical.w, physical.h);
    }
}

static void test_projection(void)
{
    static const int rotations[] = { 0, 90, 180, 270 };
    size_t n;

    for (n = 0; n < sizeof(rotations) / sizeof(rotations[0]); ++n) {
        const int rotation = rotations[n];
        float m[16];
        float x0, y0, x1, y1;

        KMSDRM_PreRotationProjection(rotation, 1280, 720, m);
        x0 = m[0] * 0.0f + m[4] * 0.0f + m[12];
        y0 = m[1] * 0.0f + m[5] * 0.0f + m[13];
        x1 = m[0] * 1280.0f + m[4] * 720.0f + m[12];
        y1 = m[1] * 1280.0f + m[5] * 720.0f + m[13];
        if (rotation == 0) {
            CHECK(closef(x0, -1.0f) && closef(y0, 1.0f) &&
                      closef(x1, 1.0f) && closef(y1, -1.0f),
                  "identity projection is the negative control");
        } else if (rotation == 90) {
            CHECK(closef(x0, -1.0f) && closef(y0, -1.0f) &&
                      closef(x1, 1.0f) && closef(y1, 1.0f),
                  "90-degree projection maps logical diagonal to panel diagonal");
        } else if (rotation == 180) {
            CHECK(closef(x0, 1.0f) && closef(y0, -1.0f) &&
                      closef(x1, -1.0f) && closef(y1, 1.0f),
                  "180-degree projection reverses both axes");
        } else {
            CHECK(closef(x0, 1.0f) && closef(y0, 1.0f) &&
                      closef(x1, -1.0f) && closef(y1, -1.0f),
                  "270-degree projection maps logical diagonal to panel diagonal");
        }
    }
}

static void test_input(void)
{
    float x, y;

    KMSDRM_PreRotationPhysicalToLogicalNormalized(90, 0.25f, 0.75f, &x, &y);
    CHECK(closef(x, 0.25f) && closef(y, 0.25f),
          "90-degree absolute input is mapped back to logical coordinates");
    KMSDRM_PreRotationPhysicalDeltaToLogical(90, 3.0f, 5.0f, &x, &y);
    CHECK(closef(x, -5.0f) && closef(y, 3.0f),
          "90-degree relative input rotates without translation");

    KMSDRM_PreRotationPhysicalToLogicalNormalized(270, 0.25f, 0.75f, &x, &y);
    CHECK(closef(x, 0.75f) && closef(y, 0.75f),
          "270-degree absolute input is mapped back to logical coordinates");
    KMSDRM_PreRotationPhysicalDeltaToLogical(0, 3.0f, 5.0f, &x, &y);
    CHECK(closef(x, 3.0f) && closef(y, 5.0f),
          "unrotated relative input is the negative control");
}

static void fill_physical_90(unsigned char *pixels, int pitch)
{
    int px, py;

    memset(pixels, 0xa5, (size_t)pitch * 4);
    for (py = 0; py < 4; ++py) {
        for (px = 0; px < 3; ++px) {
            const int lx = 3 - py;
            const int ly = px;
            pixels[py * pitch + px] = (unsigned char)(10 * ly + lx);
        }
    }
}

static void test_readback_copy(void)
{
    unsigned char physical[5 * 4];
    unsigned char logical[6 * 3];
    unsigned char identity[6 * 3];
    int x, y;

    fill_physical_90(physical, 5);
    memset(logical, 0x5a, sizeof(logical));
    KMSDRM_PreRotationCopyPhysicalToLogical(90, 4, 3, 1,
                                            physical, 5, logical, 6);
    for (y = 0; y < 3; ++y) {
        for (x = 0; x < 4; ++x) {
            CHECK(logical[y * 6 + x] == (unsigned char)(10 * y + x),
                  "90-degree readback logical pixel (%d,%d)", x, y);
        }
        CHECK(logical[y * 6 + 4] == 0x5a && logical[y * 6 + 5] == 0x5a,
              "90-degree readback preserves destination pitch sentinel row %d", y);
    }

    memset(identity, 0x3c, sizeof(identity));
    KMSDRM_PreRotationCopyPhysicalToLogical(0, 4, 3, 1,
                                            logical, 6, identity, 6);
    for (y = 0; y < 3; ++y) {
        CHECK(memcmp(identity + y * 6, logical + y * 6, 4) == 0 &&
                  identity[y * 6 + 4] == 0x3c && identity[y * 6 + 5] == 0x3c,
              "identity readback is the negative control and preserves pitch row %d", y);
    }
}

int main(void)
{
    test_rects();
    test_projection();
    test_input();
    test_readback_copy();
    printf("RESULT: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
