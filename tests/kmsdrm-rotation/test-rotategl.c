/*
 * tsp-mc9m.41.924.16.12: the KMSDRM rotate pass (SDL_kmsdrmrotategl.h, the code
 * the backend runs) on a real GLES2 implementation, Mesa llvmpipe through EGL
 * surfaceless, drawing into a pbuffer. A pbuffer is a window-system
 * framebuffer, like the GBM window surface on the device: its top row is GL's
 * top row. Every one of the 1280x720 landscape texels carries a unique colour,
 * and every panel pixel is checked against pf-framehost's pixel table for the
 * same connector property, for 90 ("Left Side Up"), 270 ("Right Side Up"),
 * 180 ("Upside Down") and 0 ("Normal", the negative control).
 *
 * tsp-mc9m.41.924.16.13.3: the same check for every SDL_KMSDRM_ROTATE_EXPERIMENT
 * pass. loadclear and twiddle must be the ordinary image, pixel for pixel;
 * sample0 must be the unrotated, squeezed image; clear must be solid grey. The
 * experiment names parse as documented and unknown names are refused.
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

static uint16_t rgb565_of(int lx, int ly)
{
    const unsigned r = (unsigned)lx * 31u / (LW - 1);
    const unsigned g = (unsigned)ly * 63u / (LH - 1);
    const unsigned b = ((unsigned)lx / 17u + (unsigned)ly / 11u) & 31u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static int rgb565_matches(uint16_t value, const unsigned char *rgba)
{
    const unsigned r = (value >> 11) & 31u;
    const unsigned g = (value >> 5) & 63u;
    const unsigned b = value & 31u;
    const unsigned er = (r << 3) | (r >> 2);
    const unsigned eg = (g << 2) | (g >> 4);
    const unsigned eb = (b << 3) | (b >> 2);
    return abs((int)rgba[0] - (int)er) <= 1 && abs((int)rgba[1] - (int)eg) <= 1 &&
           abs((int)rgba[2] - (int)eb) <= 1;
}

/* sample0: the 0-degree quad over the panel. Pixel (x, y) is centred at
   ((x + 0.5) / pw, (y + 0.5) / ph) and NEAREST picks the texel under it. The
   interpolated coordinate can land exactly on a texel edge, so allow one texel
   either way. */
static int sample0_matches(int pw, int ph, int x, int y, uint32_t got)
{
    const int ex = (int)(((double)x + 0.5) * LW / pw);
    const int ey = (int)(((double)y + 0.5) * LH / ph);
    int dx, dy;

    for (dy = -1; dy <= 1; ++dy) {
        for (dx = -1; dx <= 1; ++dx) {
            const int lx = ex + dx, ly = ey + dy;
            if (lx >= 0 && lx < LW && ly >= 0 && ly < LH && got == code_of(lx, ly)) {
                return 1;
            }
        }
    }
    return 0;
}

static int run(EGLDisplay dpy, EGLConfig config, EGLContext ctx, int rotation,
               KMSDRM_RotateExperiment experiment, int rgb565, unsigned char *capture)
{
    const char *name = KMSDRM_RotateGL_ExperimentName(experiment);
    const char *source_name = rgb565 ? "rgb565" : "rgba8888";
    KMSDRM_RotateGLExp exp = { 0 };
    GLuint draw_texture, copy_texture = 0, read_fbo = 0;
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
        printf("FAIL: rotation %d (%s): no %dx%d pbuffer (EGL error 0x%x)\n", rotation, name, pw, ph, eglGetError());
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

    src = malloc((size_t)LW * LH * (rgb565 ? 2 : 4));
    out = malloc((size_t)pw * ph * 4);
    if (!src || !out) {
        printf("FAIL: out of memory\n");
        return 1;
    }
    // Memory row 0 is the landscape image's top row, as in an application's GBM buffer.
    for (y = 0; y < LH; ++y) {
        for (x = 0; x < LW; ++x) {
            if (rgb565) {
                ((uint16_t *)src)[(size_t)y * LW + x] = rgb565_of(x, y);
            } else {
                const uint32_t code = code_of(x, y);
                unsigned char *p = &src[((size_t)y * LW + x) * 4];
                p[0] = (unsigned char)(code & 0xff);
                p[1] = (unsigned char)((code >> 8) & 0xff);
                p[2] = (unsigned char)((code >> 16) & 0xff);
                p[3] = 0xff;
            }
        }
    }
    gl.GenTextures(1, &texture);
    gl.BindTexture(GL_TEXTURE_2D, texture);
    PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (rgb565) {
        TexImage2D(GL_TEXTURE_2D, 0, GL_RGB, LW, LH, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, src);
    } else {
        TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, LW, LH, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
    }

    ClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    Clear(GL_COLOR_BUFFER_BIT);
    draw_texture = texture;
    if (experiment != KMSDRM_ROTATE_EXPERIMENT_NONE && !KMSDRM_RotateGL_LoadExp(&exp, load_proc, NULL)) {
        printf("FAIL: rotation %d (%s): the experiment entry points are unavailable\n", rotation, name);
        return 1;
    }
    if (experiment == KMSDRM_ROTATE_EXPERIMENT_TWIDDLE) {
        // As the backend does: a copy texture, a read framebuffer on the source, a row-wise copy.
        copy_texture = KMSDRM_RotateGL_CreateCopyTexture(&gl, &exp, LW, LH);
        read_fbo = copy_texture ? KMSDRM_RotateGL_CreateReadFramebuffer(&gl, &exp, texture) : 0;
        if (!copy_texture || !read_fbo || !KMSDRM_RotateGL_CopyToTexture(&gl, &exp, read_fbo, copy_texture, LW, LH)) {
            printf("FAIL: rotation %d (twiddle): copy texture %u, read framebuffer %u, or the copy failed\n",
                   rotation, copy_texture, read_fbo);
            ++failures;
        }
        draw_texture = copy_texture;
        // The source must no longer matter: scribble over it before the draw.
        memset(src, 0, (size_t)LW * LH * 4);
        gl.BindTexture(GL_TEXTURE_2D, texture);
        TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, LW, LH, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
    }
    KMSDRM_RotateGL_DrawExperiment(&gl, experiment == KMSDRM_ROTATE_EXPERIMENT_NONE ? NULL : &exp, experiment,
                                   draw_texture, rotation, pw, ph);
    gl.Finish();
    PixelStorei(GL_PACK_ALIGNMENT, 1);
    ReadPixels(0, 0, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, out);
    error = gl.GetError();
    if (error != GL_NO_ERROR) {
        printf("FAIL: rotation %d (%s): GL error 0x%x\n", rotation, name, error);
        ++failures;
    }

    // glReadPixels row r is GL row r from the bottom: panel row ph - 1 - r.
    for (y = 0; y < ph; ++y) {
        for (x = 0; x < pw; ++x) {
            const unsigned char *p = &out[((size_t)(ph - 1 - y) * pw + x) * 4];
            const uint32_t got = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
            int lx, ly, match;
            expected_source(rotation, x, y, &lx, &ly);
            if (rgb565) {
                match = rgb565_matches(rgb565_of(lx, ly), p);
            } else switch (experiment) {
            case KMSDRM_ROTATE_EXPERIMENT_CLEAR:
                match = p[0] == 0x20 && p[1] == 0x20 && p[2] == 0x20;
                break;
            case KMSDRM_ROTATE_EXPERIMENT_SAMPLE0:
                match = sample0_matches(pw, ph, x, y, got);
                break;
            default:
                match = got == code_of(lx, ly);
                break;
            }
            if (!match || p[3] != 0xff) {
                if (mismatches < 5) {
                    printf("  rotation %d (%s, %s): panel (%d,%d) shows code %u (rgba %u,%u,%u,%u); "
                           "ordinary pass: landscape (%d,%d) code %u rgb565 0x%04x\n",
                           rotation, name, source_name, x, y, got, p[0], p[1], p[2], p[3], lx, ly,
                           code_of(lx, ly), rgb565_of(lx, ly));
                }
                ++mismatches;
            }
            if (!rgb565 && got == code_of(0, 0)) {
                tl_x = x;
                tl_y = y;
            }
        }
    }
    printf("%s: rotation %d (%s, %s): %dx%d landscape -> %dx%d panel, %d of %d pixels mismatched; "
           "landscape top-left is at panel (%d,%d)\n",
           mismatches ? "FAIL" : "ok", rotation, name, source_name, LW, LH, pw, ph, mismatches, pw * ph, tl_x, tl_y);
    if (mismatches) {
        ++failures;
    }
    if (capture) {
        memcpy(capture, out, (size_t)pw * ph * 4);
    }

    if (texture) {
        gl.DeleteTextures(1, &texture);
    }
    if (copy_texture) {
        gl.DeleteTextures(1, &copy_texture);
    }
    if (read_fbo) {
        PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers =
            (PFNGLDELETEFRAMEBUFFERSPROC)eglGetProcAddress("glDeleteFramebuffers");
        if (DeleteFramebuffers) {
            DeleteFramebuffers(1, &read_fbo);
        }
    }
    KMSDRM_RotateGL_Fini(&gl);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(dpy, surface);
    free(src);
    free(out);
    return 0;
}

// SDL_KMSDRM_ROTATE_EXPERIMENT values, as KMSDRM_RotateGL_ParseExperiment reads them.
static void check_experiment_names(void)
{
    static const struct
    {
        const char *value;
        KMSDRM_RotateExperiment experiment;
        int recognised;
    } table[] = {
        { NULL, KMSDRM_ROTATE_EXPERIMENT_NONE, 1 },
        { "", KMSDRM_ROTATE_EXPERIMENT_NONE, 1 },
        { "sample0", KMSDRM_ROTATE_EXPERIMENT_SAMPLE0, 1 },
        { "clear", KMSDRM_ROTATE_EXPERIMENT_CLEAR, 1 },
        { "loadclear", KMSDRM_ROTATE_EXPERIMENT_LOADCLEAR, 1 },
        { "twiddle", KMSDRM_ROTATE_EXPERIMENT_TWIDDLE, 1 },
        { "Twiddle", KMSDRM_ROTATE_EXPERIMENT_NONE, 0 },
        { "twiddle ", KMSDRM_ROTATE_EXPERIMENT_NONE, 0 },
        { "clea", KMSDRM_ROTATE_EXPERIMENT_NONE, 0 },
        { "none", KMSDRM_ROTATE_EXPERIMENT_NONE, 0 },
        { "1", KMSDRM_ROTATE_EXPERIMENT_NONE, 0 },
    };
    size_t i;
    int bad = 0;

    for (i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        int recognised = -1;
        const KMSDRM_RotateExperiment got = KMSDRM_RotateGL_ParseExperiment(table[i].value, &recognised);
        if (got != table[i].experiment || recognised != table[i].recognised) {
            printf("FAIL: SDL_KMSDRM_ROTATE_EXPERIMENT '%s' parsed as %s (recognised %d), expected %s (recognised %d)\n",
                   table[i].value ? table[i].value : "(unset)", KMSDRM_RotateGL_ExperimentName(got), recognised,
                   KMSDRM_RotateGL_ExperimentName(table[i].experiment), table[i].recognised);
            ++bad;
        }
    }
    printf("%s: %d SDL_KMSDRM_ROTATE_EXPERIMENT values parse as documented\n", bad ? "FAIL" : "ok",
           (int)(sizeof(table) / sizeof(table[0])) - bad);
    failures += bad;
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
    static const KMSDRM_RotateExperiment diagnostic_experiments[] = {
        KMSDRM_ROTATE_EXPERIMENT_TWIDDLE, KMSDRM_ROTATE_EXPERIMENT_SAMPLE0, KMSDRM_ROTATE_EXPERIMENT_CLEAR
    };
    size_t e;
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy;
    EGLConfig config;
    EGLContext ctx;
    EGLint count = 0;
    unsigned char *ordinary;
    unsigned char *elided;
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

    ordinary = malloc((size_t)LW * LH * 4);
    elided = malloc((size_t)LW * LH * 4);
    if (!ordinary || !elided) {
        printf("FAIL: out of memory for load-elision identity controls\n");
        return 2;
    }

    check_experiment_names();
    for (i = 0; i < sizeof(rotations) / sizeof(rotations[0]); ++i) {
        if (run(dpy, config, ctx, rotations[i], KMSDRM_ROTATE_EXPERIMENT_NONE, 0, ordinary) != 0 ||
            run(dpy, config, ctx, rotations[i], KMSDRM_ROTATE_EXPERIMENT_LOADCLEAR, 0, elided) != 0) {
            return 2;
        }
        if (memcmp(ordinary, elided, (size_t)LW * LH * 4) != 0) {
            printf("FAIL: rotation %d: load elision changes panel bytes\n", rotations[i]);
            ++failures;
        } else {
            printf("ok: rotation %d: legacy and load-elided panel bytes are identical\n", rotations[i]);
        }
    }
    for (e = 0; e < sizeof(diagnostic_experiments) / sizeof(diagnostic_experiments[0]); ++e) {
        for (i = 0; i < sizeof(rotations) / sizeof(rotations[0]); ++i) {
            if (run(dpy, config, ctx, rotations[i], diagnostic_experiments[e], 0, NULL) != 0) {
                return 2;
            }
        }
    }
    for (i = 0; i < sizeof(rotations) / sizeof(rotations[0]); ++i) {
        if (run(dpy, config, ctx, rotations[i], KMSDRM_ROTATE_EXPERIMENT_NONE, 1, NULL) != 0) {
            return 2;
        }
    }
    free(elided);
    free(ordinary);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    printf("RESULT: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
