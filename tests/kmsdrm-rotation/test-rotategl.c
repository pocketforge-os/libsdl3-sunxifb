/*
 * tsp-mc9m.41.924.16.12: the KMSDRM rotate pass (SDL_kmsdrmrotategl.h, the code
 * the backend runs) on a real GLES2 implementation, Mesa llvmpipe through EGL
 * surfaceless, drawing into a pbuffer. A pbuffer is a window-system
 * framebuffer, like the GBM window surface on the device: its top row is GL's
 * top row. Every one of the 1280x720 landscape texels carries a unique colour,
 * and every panel pixel is checked against pf-framehost's pixel table for the
 * same connector property, for 90 ("Left Side Up"), 270 ("Right Side Up"),
 * 180 ("Upside Down") and 0 ("Normal", the negative control).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "SDL_kmsdrmrotategl.h"

#define LW 1280
#define LH 720

static int failures;

static KMSDRM_RotateGLProc load_proc(void *userdata, const char *name)
{
    (void)userdata;
    return (KMSDRM_RotateGLProc)eglGetProcAddress(name);
}

// pf-framehost (runtime crates/pf-framehost/src/lib.rs source_coordinates):
// the landscape pixel that panel pixel (x, y) shows. Top-left origins.
static void expected_source(int rotation, int x, int y, int *lx, int *ly)
{
    switch (rotation) {
    case 90: // "Left Side Up" -> FB_ROTATE_CCW -> Rotate270
        *lx = LW - 1 - y;
        *ly = x;
        break;
    case 270: // "Right Side Up" -> FB_ROTATE_CW -> Rotate90
        *lx = y;
        *ly = LH - 1 - x;
        break;
    case 180: // "Upside Down"
        *lx = LW - 1 - x;
        *ly = LH - 1 - y;
        break;
    default:
        *lx = x;
        *ly = y;
        break;
    }
}

static uint32_t code_of(int lx, int ly)
{
    return (uint32_t)(ly * LW + lx) + 1u; // never 0 (black) or the magenta clear
}

static int run(EGLDisplay dpy, EGLConfig config, EGLContext ctx, int rotation)
{
    const int swaps = rotation == 90 || rotation == 270;
    const int pw = swaps ? LH : LW, ph = swaps ? LW : LH;
    const EGLint pbuffer_attribs[] = { EGL_WIDTH, pw, EGL_HEIGHT, ph, EGL_NONE };
    PFNGLTEXIMAGE2DPROC TexImage2D = (PFNGLTEXIMAGE2DPROC)eglGetProcAddress("glTexImage2D");
    PFNGLREADPIXELSPROC ReadPixels = (PFNGLREADPIXELSPROC)eglGetProcAddress("glReadPixels");
    PFNGLPIXELSTOREIPROC PixelStorei = (PFNGLPIXELSTOREIPROC)eglGetProcAddress("glPixelStorei");
    PFNGLCLEARCOLORPROC ClearColor = (PFNGLCLEARCOLORPROC)eglGetProcAddress("glClearColor");
    PFNGLCLEARPROC Clear = (PFNGLCLEARPROC)eglGetProcAddress("glClear");
    KMSDRM_RotateGL gl = { 0 };
    EGLSurface surface;
    unsigned char *src, *out;
    GLuint texture = 0;
    int x, y, mismatches = 0, tl_x = -1, tl_y = -1;
    GLenum error;

    surface = eglCreatePbufferSurface(dpy, config, pbuffer_attribs);
    if (surface == EGL_NO_SURFACE || !eglMakeCurrent(dpy, surface, surface, ctx)) {
        printf("FAIL: rotation %d: no %dx%d pbuffer (EGL error 0x%x)\n", rotation, pw, ph, eglGetError());
        return 1;
    }
    if (!TexImage2D || !ReadPixels || !PixelStorei || !ClearColor || !Clear ||
        !KMSDRM_RotateGL_Load(&gl, load_proc, NULL) || !KMSDRM_RotateGL_Init(&gl)) {
        printf("FAIL: rotation %d: GLES2 entry points or the rotate program are unavailable\n", rotation);
        KMSDRM_RotateGL_Fini(&gl); // safe on an empty or program-less table
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(dpy, surface);
        return 1;
    }

    src = malloc((size_t)LW * LH * 4);
    out = malloc((size_t)pw * ph * 4);
    if (!src || !out) {
        printf("FAIL: out of memory\n");
        return 1;
    }
    // Memory row 0 is the landscape image's top row, as in an application's GBM buffer.
    for (y = 0; y < LH; ++y) {
        for (x = 0; x < LW; ++x) {
            const uint32_t code = code_of(x, y);
            unsigned char *p = &src[((size_t)y * LW + x) * 4];
            p[0] = (unsigned char)(code & 0xff);
            p[1] = (unsigned char)((code >> 8) & 0xff);
            p[2] = (unsigned char)((code >> 16) & 0xff);
            p[3] = 0xff;
        }
    }
    gl.GenTextures(1, &texture);
    gl.BindTexture(GL_TEXTURE_2D, texture);
    PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, LW, LH, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);

    ClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    Clear(GL_COLOR_BUFFER_BIT);
    KMSDRM_RotateGL_Draw(&gl, texture, rotation, pw, ph);
    gl.Finish();
    PixelStorei(GL_PACK_ALIGNMENT, 1);
    ReadPixels(0, 0, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, out);
    error = gl.GetError();
    if (error != GL_NO_ERROR) {
        printf("FAIL: rotation %d: GL error 0x%x\n", rotation, error);
        ++failures;
    }

    // glReadPixels row r is GL row r from the bottom: panel row ph - 1 - r.
    for (y = 0; y < ph; ++y) {
        for (x = 0; x < pw; ++x) {
            const unsigned char *p = &out[((size_t)(ph - 1 - y) * pw + x) * 4];
            const uint32_t got = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
            int lx, ly;
            expected_source(rotation, x, y, &lx, &ly);
            if (got != code_of(lx, ly) || p[3] != 0xff) {
                if (mismatches < 5) {
                    printf("  rotation %d: panel (%d,%d) shows code %u, expected landscape (%d,%d) code %u\n",
                           rotation, x, y, got, lx, ly, code_of(lx, ly));
                }
                ++mismatches;
            }
            if (got == code_of(0, 0)) {
                tl_x = x;
                tl_y = y;
            }
        }
    }
    printf("%s: rotation %d: %dx%d landscape -> %dx%d panel, %d of %d pixels mismatched; "
           "landscape top-left is at panel (%d,%d)\n",
           mismatches ? "FAIL" : "ok", rotation, LW, LH, pw, ph, mismatches, pw * ph, tl_x, tl_y);
    if (mismatches) {
        ++failures;
    }

    if (texture) {
        gl.DeleteTextures(1, &texture);
    }
    KMSDRM_RotateGL_Fini(&gl);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(dpy, surface);
    free(src);
    free(out);
    return 0;
}

int main(void)
{
    static const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    static const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    static const int rotations[] = { 90, 270, 180, 0 };
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy;
    EGLConfig config;
    EGLContext ctx;
    EGLint count = 0;
    size_t i;

    if (!get_platform_display) {
        printf("FAIL: eglGetPlatformDisplayEXT is unavailable\n");
        return 2;
    }
    dpy = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API) ||
        !eglChooseConfig(dpy, config_attribs, &config, 1, &count) || count < 1) {
        printf("FAIL: no surfaceless GLES2 pbuffer config (EGL error 0x%x)\n", eglGetError());
        return 2;
    }
    ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, context_attribs);
    if (ctx == EGL_NO_CONTEXT) {
        printf("FAIL: no GLES2 context (EGL error 0x%x)\n", eglGetError());
        return 2;
    }
    printf("EGL vendor: %s\n", eglQueryString(dpy, EGL_VENDOR));

    for (i = 0; i < sizeof(rotations) / sizeof(rotations[0]); ++i) {
        if (run(dpy, config, ctx, rotations[i]) != 0) {
            return 2;
        }
    }
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    printf("RESULT: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
