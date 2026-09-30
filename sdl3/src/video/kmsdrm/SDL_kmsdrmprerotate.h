/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty. In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the restrictions in the SDL source distribution.
*/

/* PocketForge: dependency-free geometry for drawing an SDL_Renderer frame
   directly into a panel-native KMSDRM surface. Tests include this exact file. */

#ifndef SDL_kmsdrmprerotate_h_
#define SDL_kmsdrmprerotate_h_

#include "SDL_kmsdrmorientation.h"

#define KMSDRM_PREROTATION_WINDOW_PROPERTY "SDL.window.KMSDRM.pocketforge.renderer_prerotation"
#define KMSDRM_PREROTATION_ACTIVE_PROPERTY "SDL.window.KMSDRM.pocketforge.renderer_prerotation_active"
#define KMSDRM_PRESENT_ROTATION_WINDOW_PROPERTY "SDL.window.KMSDRM.pocketforge.present_rotation"

typedef struct KMSDRM_PreRotationRect
{
    int x;
    int y;
    int w;
    int h;
} KMSDRM_PreRotationRect;

static inline void KMSDRM_PreRotationPhysicalSize(int rotation, int lw, int lh, int *pw, int *ph)
{
    if (KMSDRM_RotationSwapsAxes(rotation)) {
        *pw = lh;
        *ph = lw;
    } else {
        *pw = lw;
        *ph = lh;
    }
}

/* Rectangles use half-open pixel edges, unlike the pixel-index helpers in
   SDL_kmsdrmorientation.h. This keeps widths exact for viewport/scissor/read. */
static inline void KMSDRM_PreRotationLogicalRectToPhysical(int rotation, int lw, int lh,
                                                           const KMSDRM_PreRotationRect *logical,
                                                           KMSDRM_PreRotationRect *physical)
{
    switch (rotation) {
    case 90:
        physical->x = logical->y;
        physical->y = lw - logical->x - logical->w;
        physical->w = logical->h;
        physical->h = logical->w;
        break;
    case 180:
        physical->x = lw - logical->x - logical->w;
        physical->y = lh - logical->y - logical->h;
        physical->w = logical->w;
        physical->h = logical->h;
        break;
    case 270:
        physical->x = lh - logical->y - logical->h;
        physical->y = logical->x;
        physical->w = logical->h;
        physical->h = logical->w;
        break;
    default:
        *physical = *logical;
        break;
    }
}

/* Column-major matrix for GLES2's u_projection. Input coordinates are local
   to a logical viewport; GL viewport placement supplies its physical origin. */
static inline void KMSDRM_PreRotationProjection(int rotation, int lw, int lh, float matrix[16])
{
    int i;

    for (i = 0; i < 16; ++i) {
        matrix[i] = 0.0f;
    }
    matrix[10] = 1.0f;
    matrix[15] = 1.0f;

    switch (rotation) {
    case 90:
        matrix[1] = 2.0f / (float)lw;
        matrix[4] = 2.0f / (float)lh;
        matrix[12] = -1.0f;
        matrix[13] = -1.0f;
        break;
    case 180:
        matrix[0] = -2.0f / (float)lw;
        matrix[5] = 2.0f / (float)lh;
        matrix[12] = 1.0f;
        matrix[13] = -1.0f;
        break;
    case 270:
        matrix[1] = -2.0f / (float)lw;
        matrix[4] = -2.0f / (float)lh;
        matrix[12] = 1.0f;
        matrix[13] = 1.0f;
        break;
    default:
        matrix[0] = 2.0f / (float)lw;
        matrix[5] = -2.0f / (float)lh;
        matrix[12] = -1.0f;
        matrix[13] = 1.0f;
        break;
    }
}

static inline void KMSDRM_PreRotationPhysicalToLogicalNormalized(int rotation, float px, float py,
                                                                 float *lx, float *ly)
{
    switch (rotation) {
    case 90:
        *lx = 1.0f - py;
        *ly = px;
        break;
    case 180:
        *lx = 1.0f - px;
        *ly = 1.0f - py;
        break;
    case 270:
        *lx = py;
        *ly = 1.0f - px;
        break;
    default:
        *lx = px;
        *ly = py;
        break;
    }
}

static inline void KMSDRM_PreRotationPhysicalDeltaToLogical(int rotation, float pdx, float pdy,
                                                             float *ldx, float *ldy)
{
    switch (rotation) {
    case 90:
        *ldx = -pdy;
        *ldy = pdx;
        break;
    case 180:
        *ldx = -pdx;
        *ldy = -pdy;
        break;
    case 270:
        *ldx = pdy;
        *ldy = -pdx;
        break;
    default:
        *ldx = pdx;
        *ldy = pdy;
        break;
    }
}

/* Copy logical top-down pixels into the panel-native orientation. Cursor BOs
   and primary-plane readback deliberately share the exact pixel-index map in
   SDL_kmsdrmorientation.h instead of maintaining another rotation table. */
static inline void KMSDRM_PreRotationCopyLogicalToPhysical(int rotation, int lw, int lh, int bytes_per_pixel,
                                                           const void *logical_pixels, int logical_pitch,
                                                           void *physical_pixels, int physical_pitch)
{
    const unsigned char *src = (const unsigned char *)logical_pixels;
    unsigned char *dst = (unsigned char *)physical_pixels;
    int lx, ly, byte;

    for (ly = 0; ly < lh; ++ly) {
        for (lx = 0; lx < lw; ++lx) {
            int px, py;
            KMSDRM_RotationLogicalToPhysical(rotation, lw, lh, lx, ly, &px, &py);
            for (byte = 0; byte < bytes_per_pixel; ++byte) {
                dst[py * physical_pitch + px * bytes_per_pixel + byte] =
                    src[ly * logical_pitch + lx * bytes_per_pixel + byte];
            }
        }
    }
}

/* Convert a top-down physical readback into SDL's logical top-down result.
   `lw`/`lh` describe the requested logical rectangle, not the whole window. */
static inline void KMSDRM_PreRotationCopyPhysicalToLogical(int rotation, int lw, int lh, int bytes_per_pixel,
                                                           const void *physical_pixels, int physical_pitch,
                                                           void *logical_pixels, int logical_pitch)
{
    const unsigned char *src = (const unsigned char *)physical_pixels;
    unsigned char *dst = (unsigned char *)logical_pixels;
    int lx, ly, byte;

    for (ly = 0; ly < lh; ++ly) {
        for (lx = 0; lx < lw; ++lx) {
            int px, py;
            KMSDRM_RotationLogicalToPhysical(rotation, lw, lh, lx, ly, &px, &py);
            for (byte = 0; byte < bytes_per_pixel; ++byte) {
                dst[ly * logical_pitch + lx * bytes_per_pixel + byte] =
                    src[py * physical_pitch + px * bytes_per_pixel + byte];
            }
        }
    }
}

#endif // SDL_kmsdrmprerotate_h_
