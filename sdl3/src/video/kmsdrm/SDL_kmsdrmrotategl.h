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

// Returns 1 when every entry point resolved, 0 otherwise (nothing is called).
static inline int KMSDRM_RotateGL_Load(KMSDRM_RotateGL *gl, KMSDRM_RotateGLLoader load, void *userdata)
{
#define KMSDRM_ROTATEGL_LOAD(TYPE, NAME)                        \
    gl->NAME = (TYPE)load(userdata, "gl" #NAME);                \
    if (!gl->NAME) {                                            \
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
    gl->program = 0;
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

// The rotate context must be current.
static inline void KMSDRM_RotateGL_Fini(KMSDRM_RotateGL *gl)
{
    if (gl->program) {
        gl->DeleteProgram(gl->program);
        gl->program = 0;
    }
}

#endif // SDL_kmsdrmrotategl_h_
