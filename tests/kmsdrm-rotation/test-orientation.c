/*
 * tsp-mc9m.41.924.16.12: unit tests of sdl3/src/video/kmsdrm/SDL_kmsdrmorientation.h
 * (the connector "panel orientation" -> rotated present map), with no SDL, DRM
 * or GL. Expectations are literal: the kernel's enum names, the
 * pocketforge-os/runtime pf-framehost pixel table for the same property, and
 * the panel corners, so the header is checked against sources it did not
 * write itself.
 */
#include <stdio.h>
#include <string.h>

#include "SDL_kmsdrmorientation.h"

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

/* pf-framehost (runtime 2ad0ca76 crates/pf-framehost/src/lib.rs):
   "Left Side Up" -> FB_ROTATE_CCW -> PresentRotation::Rotate270 -> (sw - 1 - y, x);
   "Right Side Up" -> FB_ROTATE_CW -> PresentRotation::Rotate90 -> (y, sh - 1 - x);
   "Upside Down" -> Rotate180 -> (sw - 1 - x, sh - 1 - y). */
static void framehost_source(const char *name, int sw, int sh, int x, int y, int *u, int *v)
{
    if (strcmp(name, "Left Side Up") == 0) {
        *u = sw - 1 - y;
        *v = x;
    } else if (strcmp(name, "Right Side Up") == 0) {
        *u = y;
        *v = sh - 1 - x;
    } else if (strcmp(name, "Upside Down") == 0) {
        *u = sw - 1 - x;
        *v = sh - 1 - y;
    } else {
        *u = x;
        *v = y;
    }
}

static void test_names(void)
{
    CHECK(KMSDRM_PanelOrientationDegrees("Normal") == 0, "\"Normal\" -> 0 (negative control)");
    CHECK(KMSDRM_PanelOrientationDegrees("Left Side Up") == 90, "\"Left Side Up\" -> 90 (DRM_MODE_ROTATE_90)");
    CHECK(KMSDRM_PanelOrientationDegrees("Upside Down") == 180, "\"Upside Down\" -> 180");
    CHECK(KMSDRM_PanelOrientationDegrees("Right Side Up") == 270, "\"Right Side Up\" -> 270 (DRM_MODE_ROTATE_270)");
    CHECK(KMSDRM_PanelOrientationDegrees(NULL) == 0, "no property -> 0");
    CHECK(KMSDRM_PanelOrientationDegrees("left side up") == 0, "names are exact (kernel spelling only)");
    CHECK(KMSDRM_PanelOrientationDegrees("Left Side Up ") == 0, "no prefix match");
    CHECK(KMSDRM_PanelOrientationDegrees("") == 0, "empty name -> 0");
}

static void test_logical_size(void)
{
    static const struct
    {
        const char *name;
        int w, h;
    } cases[] = {
        { "Left Side Up", 1280, 720 },
        { "Right Side Up", 1280, 720 },
        { "Upside Down", 720, 1280 },
        { "Normal", 720, 1280 },
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        int lw = -1, lh = -1;
        KMSDRM_RotationLogicalSize(KMSDRM_PanelOrientationDegrees(cases[i].name), 720, 1280, &lw, &lh);
        CHECK(lw == cases[i].w && lh == cases[i].h, "720x1280 panel, \"%s\": application size %dx%d (expected %dx%d)",
              cases[i].name, lw, lh, cases[i].w, cases[i].h);
    }
}

static void test_corners(void)
{
    // Where the landscape image's top-left pixel lands on the 720x1280 panel.
    static const struct
    {
        const char *name;
        int px, py;
        const char *corner;
    } cases[] = {
        { "Left Side Up", 0, 1279, "bottom-left" },
        { "Right Side Up", 719, 0, "top-right" },
        { "Upside Down", 719, 1279, "bottom-right" },
        { "Normal", 0, 0, "top-left" },
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const int rotation = KMSDRM_PanelOrientationDegrees(cases[i].name);
        int lw, lh, px = -1, py = -1;
        KMSDRM_RotationLogicalSize(rotation, 720, 1280, &lw, &lh);
        KMSDRM_RotationLogicalToPhysical(rotation, lw, lh, 0, 0, &px, &py);
        CHECK(px == cases[i].px && py == cases[i].py,
              "\"%s\": logical top-left -> panel (%d,%d), the %s corner (expected (%d,%d))",
              cases[i].name, px, py, cases[i].corner, cases[i].px, cases[i].py);
    }
}

// Every pixel of a small odd-sized image: the map equals pf-framehost's, is a
// bijection, and the inverse inverts it.
static void test_pixel_map(void)
{
    static const char *const names[] = { "Normal", "Left Side Up", "Right Side Up", "Upside Down" };
    const int lw = 7, lh = 4;
    size_t n;

    for (n = 0; n < sizeof(names) / sizeof(names[0]); ++n) {
        const int rotation = KMSDRM_PanelOrientationDegrees(names[n]);
        int pw, ph, x, y, mismatches = 0, hits[7 * 4];
        memset(hits, 0, sizeof(hits));
        // The panel is lw x lh turned by the rotation.
        pw = KMSDRM_RotationSwapsAxes(rotation) ? lh : lw;
        ph = KMSDRM_RotationSwapsAxes(rotation) ? lw : lh;
        for (y = 0; y < ph; ++y) {
            for (x = 0; x < pw; ++x) {
                int lx, ly, u, v, bx, by;
                KMSDRM_RotationPhysicalToLogical(rotation, lw, lh, x, y, &lx, &ly);
                framehost_source(names[n], lw, lh, x, y, &u, &v);
                KMSDRM_RotationLogicalToPhysical(rotation, lw, lh, lx, ly, &bx, &by);
                if (lx != u || ly != v || lx < 0 || ly < 0 || lx >= lw || ly >= lh || bx != x || by != y) {
                    ++mismatches;
                } else {
                    hits[ly * lw + lx]++;
                }
            }
        }
        for (x = 0; x < lw * lh; ++x) {
            if (hits[x] != 1) {
                ++mismatches;
            }
        }
        CHECK(mismatches == 0, "\"%s\": %dx%d pixel map equals pf-framehost's, is a bijection and inverts (%d mismatches)",
              names[n], lw, lh, mismatches);
    }
}

/* The quad's corner texture coordinates, interpolated at every pixel centre of
   the panel (the map is affine, so bilinear interpolation of the four corners is
   exact), select exactly the texel the pixel map names. */
static void test_quad(void)
{
    static const int rotations[] = { 0, 90, 180, 270 };
    const int lw = 1280, lh = 720;
    size_t n;

    for (n = 0; n < sizeof(rotations) / sizeof(rotations[0]); ++n) {
        const int rotation = rotations[n];
        const int pw = KMSDRM_RotationSwapsAxes(rotation) ? lh : lw;
        const int ph = KMSDRM_RotationSwapsAxes(rotation) ? lw : lh;
        float pos[8], uv[8];
        int x, y, mismatches = 0, pos_ok;

        KMSDRM_RotationQuad(rotation, pos, uv);
        // Strip order: panel top-left, bottom-left, top-right, bottom-right.
        pos_ok = pos[0] == -1.0f && pos[1] == 1.0f && pos[2] == -1.0f && pos[3] == -1.0f &&
                 pos[4] == 1.0f && pos[5] == 1.0f && pos[6] == 1.0f && pos[7] == -1.0f;
        CHECK(pos_ok, "rotation %d: the strip covers the panel, top-left first", rotation);

        for (y = 0; y < ph; ++y) {
            for (x = 0; x < pw; ++x) {
                const double fx = (x + 0.5) / pw, fy = (y + 0.5) / ph;
                // uv at (fx, fy) from the corners TL (0), BL (1), TR (2), BR (3).
                const double s = (1 - fx) * (1 - fy) * uv[0] + (1 - fx) * fy * uv[2] + fx * (1 - fy) * uv[4] + fx * fy * uv[6];
                const double t = (1 - fx) * (1 - fy) * uv[1] + (1 - fx) * fy * uv[3] + fx * (1 - fy) * uv[5] + fx * fy * uv[7];
                const int tx = (int)(s * lw), ty = (int)(t * lh);
                int lx, ly;
                KMSDRM_RotationPhysicalToLogical(rotation, lw, lh, x, y, &lx, &ly);
                if (tx != lx || ty != ly) {
                    if (mismatches < 3) {
                        printf("  rotation %d: panel (%d,%d) samples texel (%d,%d), map says (%d,%d)\n",
                               rotation, x, y, tx, ty, lx, ly);
                    }
                    ++mismatches;
                }
            }
        }
        CHECK(mismatches == 0, "rotation %d: every panel pixel centre samples the mapped texel (%d mismatches)",
              rotation, mismatches);
    }
}

int main(void)
{
    test_names();
    test_logical_size();
    test_corners();
    test_pixel_map();
    test_quad();
    printf("RESULT: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
