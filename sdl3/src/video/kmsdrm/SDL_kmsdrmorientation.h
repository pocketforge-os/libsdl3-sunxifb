/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/* PocketForge (libsdl3-sunxifb): connector "panel orientation" -> present
   rotation. This header is deliberately dependency-free (no SDL, DRM or GL)
   so the hermetic tests under tests/kmsdrm-rotation include the exact code
   the backend runs.

   Rotation values are degrees and equal the kernel's own reading of the same
   property: drm_client_rotation() maps "Left Side Up" to DRM_MODE_ROTATE_90,
   "Upside Down" to DRM_MODE_ROTATE_180 and "Right Side Up" to
   DRM_MODE_ROTATE_270 (counter-clockwise content rotation). These are also the
   values SDL_PROP_DISPLAY_KMSDRM_PANEL_ORIENTATION_NUMBER has always used.

   Coordinates are pixel indices with a top-left origin. "Logical" is the
   landscape image the application renders (lw x lh); "physical" is the panel's
   native scanout (pw x ph, with pw == lh and ph == lw for 90/270). The pixel
   map is the one pocketforge-os/runtime pf-framehost uses for the same
   property (crates/pf-framehost/src/lib.rs source_coordinates), so the fbdev
   and GL present paths cannot disagree about a panel:
     90  ("Left Side Up"):  physical (x, y) shows logical (lw - 1 - y, x)
     270 ("Right Side Up"): physical (x, y) shows logical (y, lh - 1 - x)
     180 ("Upside Down"):   physical (x, y) shows logical (lw - 1 - x, lh - 1 - y)
     0   ("Normal"):        identity */

#ifndef SDL_kmsdrmorientation_h_
#define SDL_kmsdrmorientation_h_

static inline int KMSDRM_OrientationNameIs(const char *name, const char *expected)
{
    while (*name && *name == *expected) {
        ++name;
        ++expected;
    }
    return *name == '\0' && *expected == '\0';
}

// Enum name of the connector "panel orientation" property -> degrees.
static inline int KMSDRM_PanelOrientationDegrees(const char *name)
{
    if (!name) {
        return 0;
    }
    if (KMSDRM_OrientationNameIs(name, "Left Side Up")) {
        return 90;
    }
    if (KMSDRM_OrientationNameIs(name, "Upside Down")) {
        return 180;
    }
    if (KMSDRM_OrientationNameIs(name, "Right Side Up")) {
        return 270;
    }
    return 0; // "Normal", or a name this code does not know: no rotation.
}

static inline int KMSDRM_RotationIsValid(int rotation)
{
    return rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270;
}

static inline int KMSDRM_RotationSwapsAxes(int rotation)
{
    return rotation == 90 || rotation == 270;
}

// Physical scanout size -> the size the application sees.
static inline void KMSDRM_RotationLogicalSize(int rotation, int pw, int ph, int *lw, int *lh)
{
    if (KMSDRM_RotationSwapsAxes(rotation)) {
        *lw = ph;
        *lh = pw;
    } else {
        *lw = pw;
        *lh = ph;
    }
}

// Which logical pixel the physical pixel (px, py) shows.
static inline void KMSDRM_RotationPhysicalToLogical(int rotation, int lw, int lh, int px, int py, int *lx, int *ly)
{
    switch (rotation) {
    case 90:
        *lx = lw - 1 - py;
        *ly = px;
        break;
    case 180:
        *lx = lw - 1 - px;
        *ly = lh - 1 - py;
        break;
    case 270:
        *lx = py;
        *ly = lh - 1 - px;
        break;
    default:
        *lx = px;
        *ly = py;
        break;
    }
}

// Where the logical pixel (lx, ly) lands on the panel (inverse of the above).
static inline void KMSDRM_RotationLogicalToPhysical(int rotation, int lw, int lh, int lx, int ly, int *px, int *py)
{
    switch (rotation) {
    case 90:
        *px = ly;
        *py = lw - 1 - lx;
        break;
    case 180:
        *px = lw - 1 - lx;
        *py = lh - 1 - ly;
        break;
    case 270:
        *px = lh - 1 - ly;
        *py = lx;
        break;
    default:
        *px = lx;
        *py = ly;
        break;
    }
}

/* The rotate pass draws one triangle strip over the physical target, corners
   in the order top-left, bottom-left, top-right, bottom-right of the panel.
   pos receives clip-space x,y pairs (y = +1 is the panel's top row, as for any
   window-system surface). uv receives texture coordinates into the logical
   image, with t = 0 at the logical top row: an EGLImage of a GBM buffer (and
   glTexImage2D) puts memory row 0 at t = 0, and memory row 0 of a buffer an
   application rendered through EGL is that application's top row.

   The corner mapping is the continuous form of
   KMSDRM_RotationPhysicalToLogical: with the physical corner at (X, Y) in
   [0, 1] (Y down), the logical coordinate is (X, Y) for 0, (1 - Y, X) for 90,
   (1 - X, 1 - Y) for 180 and (Y, 1 - X) for 270. Interpolated at a pixel
   centre this lands exactly on the texel centre the pixel map names, so a
   NEAREST sample is an exact copy. */
static inline void KMSDRM_RotationQuad(int rotation, float pos[8], float uv[8])
{
    static const float corner_x[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    static const float corner_y[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    int i;

    for (i = 0; i < 4; ++i) {
        const float x = corner_x[i];
        const float y = corner_y[i];
        float s, t;

        switch (rotation) {
        case 90:
            s = 1.0f - y;
            t = x;
            break;
        case 180:
            s = 1.0f - x;
            t = 1.0f - y;
            break;
        case 270:
            s = y;
            t = 1.0f - x;
            break;
        default:
            s = x;
            t = y;
            break;
        }

        pos[2 * i + 0] = 2.0f * x - 1.0f;
        pos[2 * i + 1] = 1.0f - 2.0f * y;
        uv[2 * i + 0] = s;
        uv[2 * i + 1] = t;
    }
}

#endif // SDL_kmsdrmorientation_h_
