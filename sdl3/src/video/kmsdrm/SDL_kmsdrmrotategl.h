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

/* PocketForge (libsdl3-sunxifb): the GLES2 rotate pass of the KMSDRM rotated
   present. It copies the application's logical frame (a texture) into the
   panel-native present surface with one full-screen, NEAREST, 1:1 texel draw.

   Like SDL_kmsdrmorientation.h this is dependency-free apart from the GLES2
   types and PFNGL...PROC typedefs, which the includer provides (SDL through
   <SDL3/SDL_opengles2.h>, the hermetic llvmpipe test through <GLES2/gl2.h>),
   so the test draws with exactly the code the backend runs. It runs only in
   the backend's own GLES2 context, never in the application's. */

#ifndef SDL_kmsdrmrotategl_h_
#define SDL_kmsdrmrotategl_h_

#include "SDL_kmsdrmorientation.h"

#define KMSDRM_ROTATEGL_ATTRIB_POS 0
#define KMSDRM_ROTATEGL_ATTRIB_UV  1

typedef void (*KMSDRM_RotateGLProc)(void);
typedef KMSDRM_RotateGLProc (*KMSDRM_RotateGLLoader)(void *userdata, const char *name);

typedef struct KMSDRM_RotateGL
{
    PFNGLACTIVETEXTUREPROC ActiveTexture;
    PFNGLATTACHSHADERPROC AttachShader;
    PFNGLBINDATTRIBLOCATIONPROC BindAttribLocation;
    PFNGLBINDTEXTUREPROC BindTexture;
    PFNGLCOMPILESHADERPROC CompileShader;
    PFNGLCREATEPROGRAMPROC CreateProgram;
    PFNGLCREATESHADERPROC CreateShader;
    PFNGLDELETEPROGRAMPROC DeleteProgram;
    PFNGLDELETESHADERPROC DeleteShader;
    PFNGLDELETETEXTURESPROC DeleteTextures;
    PFNGLDISABLEPROC Disable;
    PFNGLDRAWARRAYSPROC DrawArrays;
    PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray;
    PFNGLFINISHPROC Finish;
    PFNGLFLUSHPROC Flush;
    PFNGLGENTEXTURESPROC GenTextures;
    PFNGLGETERRORPROC GetError;
    PFNGLGETPROGRAMIVPROC GetProgramiv;
    PFNGLGETSHADERIVPROC GetShaderiv;
    PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation;
    PFNGLLINKPROGRAMPROC LinkProgram;
    PFNGLSHADERSOURCEPROC ShaderSource;
    PFNGLTEXPARAMETERIPROC TexParameteri;
    PFNGLUNIFORM1IPROC Uniform1i;
    PFNGLUSEPROGRAMPROC UseProgram;
    PFNGLVERTEXATTRIBPOINTERPROC VertexAttribPointer;
    PFNGLVIEWPORTPROC Viewport;

    GLuint program;
} KMSDRM_RotateGL;

/* Resolves every entry point. Returns 1 when all of them resolved, 0 otherwise.
   The table is all-or-nothing: entry points are resolved into a local table,
   which is installed only when complete, so on failure *gl is all NULL with
   program 0 (never partly loaded or uninitialized), and KMSDRM_RotateGL_Fini
   is safe on it. No GL function is called. */
static inline int KMSDRM_RotateGL_Load(KMSDRM_RotateGL *gl, KMSDRM_RotateGLLoader load, void *userdata)
{
    const KMSDRM_RotateGL empty = { 0 };
    KMSDRM_RotateGL loaded = { 0 };

    *gl = empty;
#define KMSDRM_ROTATEGL_LOAD(TYPE, NAME)                        \
    loaded.NAME = (TYPE)load(userdata, "gl" #NAME);             \
    if (!loaded.NAME) {                                         \
        return 0;                                               \
    }
    KMSDRM_ROTATEGL_LOAD(PFNGLACTIVETEXTUREPROC, ActiveTexture);
    KMSDRM_ROTATEGL_LOAD(PFNGLATTACHSHADERPROC, AttachShader);
    KMSDRM_ROTATEGL_LOAD(PFNGLBINDATTRIBLOCATIONPROC, BindAttribLocation);
    KMSDRM_ROTATEGL_LOAD(PFNGLBINDTEXTUREPROC, BindTexture);
    KMSDRM_ROTATEGL_LOAD(PFNGLCOMPILESHADERPROC, CompileShader);
    KMSDRM_ROTATEGL_LOAD(PFNGLCREATEPROGRAMPROC, CreateProgram);
    KMSDRM_ROTATEGL_LOAD(PFNGLCREATESHADERPROC, CreateShader);
    KMSDRM_ROTATEGL_LOAD(PFNGLDELETEPROGRAMPROC, DeleteProgram);
    KMSDRM_ROTATEGL_LOAD(PFNGLDELETESHADERPROC, DeleteShader);
    KMSDRM_ROTATEGL_LOAD(PFNGLDELETETEXTURESPROC, DeleteTextures);
    KMSDRM_ROTATEGL_LOAD(PFNGLDISABLEPROC, Disable);
    KMSDRM_ROTATEGL_LOAD(PFNGLDRAWARRAYSPROC, DrawArrays);
    KMSDRM_ROTATEGL_LOAD(PFNGLENABLEVERTEXATTRIBARRAYPROC, EnableVertexAttribArray);
    KMSDRM_ROTATEGL_LOAD(PFNGLFINISHPROC, Finish);
    KMSDRM_ROTATEGL_LOAD(PFNGLFLUSHPROC, Flush);
    KMSDRM_ROTATEGL_LOAD(PFNGLGENTEXTURESPROC, GenTextures);
    KMSDRM_ROTATEGL_LOAD(PFNGLGETERRORPROC, GetError);
    KMSDRM_ROTATEGL_LOAD(PFNGLGETPROGRAMIVPROC, GetProgramiv);
    KMSDRM_ROTATEGL_LOAD(PFNGLGETSHADERIVPROC, GetShaderiv);
    KMSDRM_ROTATEGL_LOAD(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation);
    KMSDRM_ROTATEGL_LOAD(PFNGLLINKPROGRAMPROC, LinkProgram);
    KMSDRM_ROTATEGL_LOAD(PFNGLSHADERSOURCEPROC, ShaderSource);
    KMSDRM_ROTATEGL_LOAD(PFNGLTEXPARAMETERIPROC, TexParameteri);
    KMSDRM_ROTATEGL_LOAD(PFNGLUNIFORM1IPROC, Uniform1i);
    KMSDRM_ROTATEGL_LOAD(PFNGLUSEPROGRAMPROC, UseProgram);
    KMSDRM_ROTATEGL_LOAD(PFNGLVERTEXATTRIBPOINTERPROC, VertexAttribPointer);
    KMSDRM_ROTATEGL_LOAD(PFNGLVIEWPORTPROC, Viewport);
#undef KMSDRM_ROTATEGL_LOAD
    loaded.program = 0;
    *gl = loaded;
    return 1;
}

static inline GLuint KMSDRM_RotateGL_CompileShader(const KMSDRM_RotateGL *gl, GLenum type, const char *source)
{
    GLint status = GL_FALSE;
    GLuint shader = gl->CreateShader(type);

    if (!shader) {
        return 0;
    }
    gl->ShaderSource(shader, 1, &source, NULL);
    gl->CompileShader(shader);
    gl->GetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        gl->DeleteShader(shader);
        return 0;
    }
    return shader;
}

// Builds the program. The rotate context must be current. Returns 1 on success.
static inline int KMSDRM_RotateGL_Init(KMSDRM_RotateGL *gl)
{
    static const char *vertex_source =
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "varying vec2 v_uv;\n"
        "void main()\n"
        "{\n"
        "    v_uv = a_uv;\n"
        "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
        "}\n";
    static const char *fragment_source =
        "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
        "precision highp float;\n"
        "#else\n"
        "precision mediump float;\n"
        "#endif\n"
        "uniform sampler2D u_tex;\n"
        "varying vec2 v_uv;\n"
        "void main()\n"
        "{\n"
        "    gl_FragColor = texture2D(u_tex, v_uv);\n"
        "}\n";
    GLint status = GL_FALSE;
    GLuint vs, fs, program;

    vs = KMSDRM_RotateGL_CompileShader(gl, GL_VERTEX_SHADER, vertex_source);
    fs = KMSDRM_RotateGL_CompileShader(gl, GL_FRAGMENT_SHADER, fragment_source);
    program = gl->CreateProgram();
    if (!vs || !fs || !program) {
        if (vs) {
            gl->DeleteShader(vs);
        }
        if (fs) {
            gl->DeleteShader(fs);
        }
        if (program) {
            gl->DeleteProgram(program);
        }
        return 0;
    }

    gl->AttachShader(program, vs);
    gl->AttachShader(program, fs);
    gl->BindAttribLocation(program, KMSDRM_ROTATEGL_ATTRIB_POS, "a_pos");
    gl->BindAttribLocation(program, KMSDRM_ROTATEGL_ATTRIB_UV, "a_uv");
    gl->LinkProgram(program);
    // Flagged for deletion; they live as long as the program.
    gl->DeleteShader(vs);
    gl->DeleteShader(fs);
    gl->GetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        gl->DeleteProgram(program);
        return 0;
    }

    gl->UseProgram(program);
    gl->Uniform1i(gl->GetUniformLocation(program, "u_tex"), 0);
    gl->program = program;
    return 1;
}

/* Draws the logical frame in `texture` rotated by `rotation` degrees into the
   current draw surface, which is the pw x ph panel-native present surface. */
static inline void KMSDRM_RotateGL_Draw(const KMSDRM_RotateGL *gl, GLuint texture, int rotation, int pw, int ph)
{
    float pos[8], uv[8];

    KMSDRM_RotationQuad(rotation, pos, uv);

    gl->Disable(GL_BLEND);
    gl->Disable(GL_CULL_FACE);
    gl->Disable(GL_DEPTH_TEST);
    gl->Disable(GL_DITHER);
    gl->Disable(GL_SCISSOR_TEST);
    gl->Disable(GL_STENCIL_TEST);
    gl->Viewport(0, 0, pw, ph);

    gl->UseProgram(gl->program);
    gl->ActiveTexture(GL_TEXTURE0);
    gl->BindTexture(GL_TEXTURE_2D, texture);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Client-side arrays (valid in GLES2): read at glDrawArrays time.
    gl->VertexAttribPointer(KMSDRM_ROTATEGL_ATTRIB_POS, 2, GL_FLOAT, GL_FALSE, 0, pos);
    gl->VertexAttribPointer(KMSDRM_ROTATEGL_ATTRIB_UV, 2, GL_FLOAT, GL_FALSE, 0, uv);
    gl->EnableVertexAttribArray(KMSDRM_ROTATEGL_ATTRIB_POS);
    gl->EnableVertexAttribArray(KMSDRM_ROTATEGL_ATTRIB_UV);
    gl->DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Deletes the program. The rotate context must be current when a program
   exists. Safe on a table KMSDRM_RotateGL_Load left empty and on one whose
   KMSDRM_RotateGL_Init failed (program 0): it then calls nothing. */
static inline void KMSDRM_RotateGL_Fini(KMSDRM_RotateGL *gl)
{
    if (gl->program && gl->DeleteProgram) {
        gl->DeleteProgram(gl->program);
    }
    gl->program = 0;
}

/* tsp-mc9m.41.924.16.13.3: device experiments on the rotate pass, selected by
   SDL_KMSDRM_ROTATE_EXPERIMENT. They exist to attribute the rotate pass's GPU
   time on the device (one boot, no rebuild per arm). An explicit selector
   overrides the default load-elision policy; unset (or empty) is not itself
   an experiment and produces no experiment witness.

     sample0    the same draw with the 0-degree quad: the landscape image
                squeezed onto the panel, unrotated. It reads every source cache
                line, row by row, over the same target and fragment count;
                only the sampling direction differs from the rotated draw.
                Diagnostic image.
     clear      a glClear of the present surface instead of the textured draw.
                Diagnostic image (solid dark grey).
     loadclear  a glClear, then the normal rotated draw: the same image, but the
                target is cleared instead of loaded.
     twiddle    the application's frame is first copied (glCopyTexSubImage2D,
                row-wise) into a texture the rotate context owns, which the
                driver may lay out optimally, and the normal rotated draw
                samples that copy. The same image. */
typedef enum KMSDRM_RotateExperiment
{
    KMSDRM_ROTATE_EXPERIMENT_NONE = 0,
    KMSDRM_ROTATE_EXPERIMENT_SAMPLE0,
    KMSDRM_ROTATE_EXPERIMENT_CLEAR,
    KMSDRM_ROTATE_EXPERIMENT_LOADCLEAR,
    KMSDRM_ROTATE_EXPERIMENT_TWIDDLE
} KMSDRM_RotateExperiment;

#define SDL_HINT_KMSDRM_ROTATE_EXPERIMENT "SDL_KMSDRM_ROTATE_EXPERIMENT"

// The clear colour of the clear and loadclear experiments: 0x20 grey, exact in 8-bit UNORM.
#define KMSDRM_ROTATEGL_CLEAR_LEVEL (32.0f / 255.0f)

#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif

static inline const char *KMSDRM_RotateGL_ExperimentName(KMSDRM_RotateExperiment experiment)
{
    switch (experiment) {
    case KMSDRM_ROTATE_EXPERIMENT_SAMPLE0:
        return "sample0";
    case KMSDRM_ROTATE_EXPERIMENT_CLEAR:
        return "clear";
    case KMSDRM_ROTATE_EXPERIMENT_LOADCLEAR:
        return "loadclear";
    case KMSDRM_ROTATE_EXPERIMENT_TWIDDLE:
        return "twiddle";
    default:
        return "none";
    }
}

/* SDL_KMSDRM_ROTATE_EXPERIMENT -> experiment. NULL and "" are no experiment
   (*recognised = 1); an unknown value is also no experiment, but with
   *recognised = 0 so the caller can say it ignored it. */
static inline KMSDRM_RotateExperiment KMSDRM_RotateGL_ParseExperiment(const char *value, int *recognised)
{
    static const KMSDRM_RotateExperiment all[] = {
        KMSDRM_ROTATE_EXPERIMENT_SAMPLE0, KMSDRM_ROTATE_EXPERIMENT_CLEAR,
        KMSDRM_ROTATE_EXPERIMENT_LOADCLEAR, KMSDRM_ROTATE_EXPERIMENT_TWIDDLE
    };
    unsigned i;

    *recognised = 1;
    if (!value || !*value) {
        return KMSDRM_ROTATE_EXPERIMENT_NONE;
    }
    for (i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        if (KMSDRM_OrientationNameIs(value, KMSDRM_RotateGL_ExperimentName(all[i]))) {
            return all[i];
        }
    }
    *recognised = 0;
    return KMSDRM_ROTATE_EXPERIMENT_NONE;
}

// The extra GLES2 entry points the experiments use; resolved only for an experiment.
typedef struct KMSDRM_RotateGLExp
{
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus;
    PFNGLCLEARPROC Clear;
    PFNGLCLEARCOLORPROC ClearColor;
    PFNGLCOPYTEXSUBIMAGE2DPROC CopyTexSubImage2D;
    PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D;
    PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
    PFNGLTEXIMAGE2DPROC TexImage2D;
} KMSDRM_RotateGLExp;

// All-or-nothing, like KMSDRM_RotateGL_Load. Returns 1 when every entry point resolved.
static inline int KMSDRM_RotateGL_LoadExp(KMSDRM_RotateGLExp *exp, KMSDRM_RotateGLLoader load, void *userdata)
{
    const KMSDRM_RotateGLExp empty = { 0 };
    KMSDRM_RotateGLExp loaded = { 0 };

    *exp = empty;
#define KMSDRM_ROTATEGL_LOAD_EXP(TYPE, NAME)                    \
    loaded.NAME = (TYPE)load(userdata, "gl" #NAME);             \
    if (!loaded.NAME) {                                         \
        return 0;                                               \
    }
    KMSDRM_ROTATEGL_LOAD_EXP(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer);
    KMSDRM_ROTATEGL_LOAD_EXP(PFNGLCHECKFRAMEBUFFERSTATUSPROC, CheckFramebufferStatus);
    KMSDRM_ROTATEGL_LOAD_EXP(PFNGLCLEARPROC, Clear);
    KMSDRM_ROTATEGL_LOAD_EXP(PFNGLCLEARCOLORPROC, ClearColor);
    KMSDRM_ROTATEGL_LOAD_EXP(PFNGLCOPYTEXSUBIMAGE2DPROC, CopyTexSubImage2D);
    KMSDRM_ROTATEGL_LOAD_EXP(PFNGLFRAMEBUFFERTEXTURE2DPROC, FramebufferTexture2D);
    KMSDRM_ROTATEGL_LOAD_EXP(PFNGLGENFRAMEBUFFERSPROC, GenFramebuffers);
    KMSDRM_ROTATEGL_LOAD_EXP(PFNGLTEXIMAGE2DPROC, TexImage2D);
#undef KMSDRM_ROTATEGL_LOAD_EXP
    *exp = loaded;
    return 1;
}

/* A w x h texture for the twiddle experiment's copy, in the current context.
   GL_BGRA_EXT first (the layout of an ARGB8888 GBM buffer, so the copy needs
   no format conversion), GL_RGBA if the stack refuses it. Returns 0 on failure. */
static inline GLuint KMSDRM_RotateGL_CreateCopyTexture(const KMSDRM_RotateGL *gl, const KMSDRM_RotateGLExp *exp,
                                                       int w, int h)
{
    static const GLenum formats[2] = { GL_BGRA_EXT, GL_RGBA };
    GLuint texture = 0;
    int i;

    gl->GetError(); // Start from a clean error state.
    gl->GenTextures(1, &texture);
    if (!texture) {
        return 0;
    }
    gl->ActiveTexture(GL_TEXTURE0);
    gl->BindTexture(GL_TEXTURE_2D, texture);
    for (i = 0; i < 2; ++i) {
        exp->TexImage2D(GL_TEXTURE_2D, 0, (GLint)formats[i], w, h, 0, formats[i], GL_UNSIGNED_BYTE, NULL);
        if (gl->GetError() == GL_NO_ERROR) {
            return texture;
        }
    }
    gl->DeleteTextures(1, &texture);
    return 0;
}

/* A framebuffer object whose colour attachment is `texture`, the read source
   of the twiddle experiment's copy. Leaves framebuffer 0 bound. Returns 0 when
   it is not complete (it is then deleted by the context, never used). */
static inline GLuint KMSDRM_RotateGL_CreateReadFramebuffer(const KMSDRM_RotateGL *gl, const KMSDRM_RotateGLExp *exp,
                                                           GLuint texture)
{
    GLuint fbo = 0;
    GLenum status;

    gl->GetError();
    exp->GenFramebuffers(1, &fbo);
    if (!fbo) {
        return 0;
    }
    exp->BindFramebuffer(GL_FRAMEBUFFER, fbo);
    exp->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    status = exp->CheckFramebufferStatus(GL_FRAMEBUFFER);
    exp->BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE || gl->GetError() != GL_NO_ERROR) {
        return 0;
    }
    return fbo;
}

/* Copies the w x h image attached to `read_fbo` into `texture`, row for row,
   and leaves framebuffer 0 (the present surface) bound. Returns 1 on success. */
static inline int KMSDRM_RotateGL_CopyToTexture(const KMSDRM_RotateGL *gl, const KMSDRM_RotateGLExp *exp,
                                                GLuint read_fbo, GLuint texture, int w, int h)
{
    gl->GetError();
    exp->BindFramebuffer(GL_FRAMEBUFFER, read_fbo);
    gl->ActiveTexture(GL_TEXTURE0);
    gl->BindTexture(GL_TEXTURE_2D, texture);
    exp->CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    exp->BindFramebuffer(GL_FRAMEBUFFER, 0);
    return gl->GetError() == GL_NO_ERROR;
}

// Fills the pw x ph present surface with the experiments' clear colour.
static inline void KMSDRM_RotateGL_ClearPanel(const KMSDRM_RotateGL *gl, const KMSDRM_RotateGLExp *exp, int pw, int ph)
{
    gl->Disable(GL_SCISSOR_TEST);
    gl->Viewport(0, 0, pw, ph);
    exp->ClearColor(KMSDRM_ROTATEGL_CLEAR_LEVEL, KMSDRM_ROTATEGL_CLEAR_LEVEL, KMSDRM_ROTATEGL_CLEAR_LEVEL, 1.0f);
    exp->Clear(GL_COLOR_BUFFER_BIT);
}

/* The rotate pass under an experiment (texture is what it samples: the copy,
   for twiddle). NONE and TWIDDLE draw exactly as KMSDRM_RotateGL_Draw; `exp`
   may be NULL only for those two. */
static inline void KMSDRM_RotateGL_DrawExperiment(const KMSDRM_RotateGL *gl, const KMSDRM_RotateGLExp *exp,
                                                  KMSDRM_RotateExperiment experiment, GLuint texture,
                                                  int rotation, int pw, int ph)
{
    switch (experiment) {
    case KMSDRM_ROTATE_EXPERIMENT_SAMPLE0:
        KMSDRM_RotateGL_Draw(gl, texture, 0, pw, ph);
        break;
    case KMSDRM_ROTATE_EXPERIMENT_CLEAR:
        KMSDRM_RotateGL_ClearPanel(gl, exp, pw, ph);
        break;
    case KMSDRM_ROTATE_EXPERIMENT_LOADCLEAR:
        KMSDRM_RotateGL_ClearPanel(gl, exp, pw, ph);
        KMSDRM_RotateGL_Draw(gl, texture, rotation, pw, ph);
        break;
    default:
        KMSDRM_RotateGL_Draw(gl, texture, rotation, pw, ph);
        break;
    }
}

#endif // SDL_kmsdrmrotategl_h_
