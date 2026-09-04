/*
  Copyright (C) 1997-2024 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* pf_gfxbench — PocketForge OWNED ramping GPU+CPU stress benchmark (bead tsp-147u.8.5).
 *
 * A ~30s escalating-load GLES2 test whose complexity GROWS over wall-clock time
 * ("3DMark-inspired": escalating stress + a visual + a score), deliberately BASIC
 * and synthetic — NOT a photoreal scene. It must produce a DEVICE-DIFFERENTIATING
 * FPS gradient: on a weak GPU the curve tanks to a low floor; on a strong device
 * the same test stays high. The relative gradient across devices is the deliverable.
 *
 * Three load axes escalate per stage (10 stages x 3s wall-clock by default):
 *   1. GPU fill      — fullscreen ADDITIVE-BLENDED overdraw layers running a
 *                      procedurally heavy fragment shader. Per-stage iteration
 *                      count is baked as a compile-time constant (GLSL ES 1.00
 *                      forbids/penalizes uniform-bounded loops on some drivers);
 *                      all stage variants are precompiled at startup. Blending
 *                      defeats PowerVR TBDR hidden-surface removal, so every
 *                      layer really shades every fragment.
 *   2. GPU geometry  — a field of rotating cubes growing geometrically per stage;
 *                      one draw call + one CPU-computed MVP per cube (draw-call +
 *                      vertex + CPU matrix pressure).
 *   3. CPU           — a particle sim (Euler integration + gravity + wall bounce +
 *                      a capped sliding-window pairwise repulsion) growing per
 *                      stage, re-uploaded to the GL each frame (real CPU work
 *                      feeding the render, plus per-frame upload pressure).
 *
 * Stages advance on WALL CLOCK, never frame count, so a device at 2 fps still
 * completes on schedule. Bounded + graceful: a hard total deadline and a
 * per-frame watchdog abort the run cleanly (partial summary, clean GL teardown)
 * rather than wedging the device.
 *
 * Machine contract (stdout; human chatter goes to SDL_Log/stderr):
 *   pf_gfxbench_version=1
 *   config duration_s=.. stages=.. stage_s=.. report_s=.. vsync_req=.. vsync_actual=.. width=.. height=..
 *   gl_vendor=..  gl_renderer=..  gl_version=..
 *   stage_config stage=N t=.. cubes=.. layers=.. iters=.. particles=..
 *   fps_sample t=.. stage=N fps=..
 *   summary start_fps=.. floor_fps=.. gradient_ratio=.. score_frames=.. frames=.. wall_s=.. stages_completed=N/M
 *   pf_gfxbench_status=complete|watchdog_abort|deadline_abort|user_abort
 *
 * Exit codes: 0 complete, 2 init failure (SDL/GLES2 unavailable), 3 aborted
 * (watchdog/deadline/user — partial summary still emitted).
 *
 * Consumed by pocketforge-automation/tests/hil-regress/tests/gfx-bench.sh
 * (pf-regress framework, spine tsp-147u.8.1).
 */

#include <SDL3/SDL_test_common.h>
#include <SDL3/SDL_main.h>

#include <stdlib.h>
#include <stdio.h>

#if defined(SDL_PLATFORM_IOS) || defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_EMSCRIPTEN) || defined(SDL_PLATFORM_LINUX)
#define HAVE_OPENGLES2
#endif

#ifdef HAVE_OPENGLES2

#include <SDL3/SDL_opengles2.h>

typedef struct GLES2_Context
{
#define SDL_PROC(ret, func, params) ret (APIENTRY *func) params;
#include "../src/render/opengles2/SDL_gles2funcs.h"
#undef SDL_PROC
} GLES2_Context;

/* --- tunables (CLI-overridable) -------------------------------------------- */

#define PF_DEF_DURATION_S      30.0
#define PF_DEF_STAGES          10
#define PF_DEF_REPORT_S        0.5
#define PF_DEF_WARMUP_S        0.5
#define PF_DEF_WATCHDOG_S      5.0
#define PF_DEF_DEADLINE_SLOP_S 20.0

#define PF_CUBES_BASE          4
#define PF_CUBES_GROWTH        2.0
#define PF_CUBES_CAP           4096
#define PF_LAYERS_CAP          64     /* fill schedule: see stage_fill_table */
#define PF_PARTICLES_BASE      500    /* particles = 500*(stage+1)^2 */
#define PF_PARTICLES_CAP       200000
#define PF_MAX_STAGES          32

static SDLTest_CommonState *state;
static SDL_GLContext gl_context = NULL;
static GLES2_Context ctx;

static double opt_duration_s = PF_DEF_DURATION_S;
static int opt_stages = PF_DEF_STAGES;
static double opt_report_s = PF_DEF_REPORT_S;
static int opt_vsync = 0; /* 0 = attempt uncapped (cross-device gradient), 1 = keep vsync */
static double opt_watchdog_s = PF_DEF_WATCHDOG_S;

/* --- deterministic PRNG (fixed seed: run-to-run reproducible layout) -------- */

static Uint32 prng_state = 0x9E3779B9u;
static float frand(void)
{
    prng_state = prng_state * 1664525u + 1013904223u;
    return (float)((prng_state >> 8) & 0xFFFFFF) / 16777215.0f;
}

/* --- tiny column-major mat4 helpers (CPU work is part of the point) --------- */

static void mat_identity(float *m)
{
    int i;
    for (i = 0; i < 16; i++) {
        m[i] = 0.0f;
    }
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat_multiply(float *r, const float *a, const float *b)
{
    /* r = a * b, column-major */
    float t[16];
    int i, j, k;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++) {
                s += a[k * 4 + j] * b[i * 4 + k];
            }
            t[i * 4 + j] = s;
        }
    }
    for (i = 0; i < 16; i++) {
        r[i] = t[i];
    }
}

static void mat_rotate(float angle_deg, float x, float y, float z, float *r)
{
    float radians, c, s, c1, length, u[3];
    int i, j;

    radians = (angle_deg * SDL_PI_F) / 180.0f;
    c = SDL_cosf(radians);
    s = SDL_sinf(radians);
    c1 = 1.0f - c;
    length = (float)SDL_sqrt(x * x + y * y + z * z);
    u[0] = x / length;
    u[1] = y / length;
    u[2] = z / length;

    for (i = 0; i < 16; i++) {
        r[i] = 0.0f;
    }
    r[15] = 1.0f;
    for (i = 0; i < 3; i++) {
        r[i * 4 + (i + 1) % 3] = u[(i + 2) % 3] * s;
        r[i * 4 + (i + 2) % 3] = -u[(i + 1) % 3] * s;
    }
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            r[i * 4 + j] += c1 * u[i] * u[j] + (i == j ? c : 0.0f);
        }
    }
}

static void mat_perspective(float fovy, float aspect, float znear, float zfar, float *r)
{
    int i;
    float f = 1.0f / SDL_tanf((fovy / 180.0f) * SDL_PI_F * 0.5f);
    for (i = 0; i < 16; i++) {
        r[i] = 0.0f;
    }
    r[0] = f / aspect;
    r[5] = f;
    r[10] = (znear + zfar) / (znear - zfar);
    r[11] = -1.0f;
    r[14] = (2.0f * znear * zfar) / (znear - zfar);
}

/* --- geometry --------------------------------------------------------------- */

/* Unit cube, 36 vertices (glDrawArrays only — glDrawElements is not in the
 * SDL_gles2funcs.h loader table), matching testgles2's non-indexed style. */
static const float cube_positions[] = {
    /* front */
    -0.5f, -0.5f,  0.5f,  0.5f, -0.5f,  0.5f,  0.5f,  0.5f,  0.5f,
    -0.5f, -0.5f,  0.5f,  0.5f,  0.5f,  0.5f, -0.5f,  0.5f,  0.5f,
    /* back */
     0.5f, -0.5f, -0.5f, -0.5f, -0.5f, -0.5f, -0.5f,  0.5f, -0.5f,
     0.5f, -0.5f, -0.5f, -0.5f,  0.5f, -0.5f,  0.5f,  0.5f, -0.5f,
    /* left */
    -0.5f, -0.5f, -0.5f, -0.5f, -0.5f,  0.5f, -0.5f,  0.5f,  0.5f,
    -0.5f, -0.5f, -0.5f, -0.5f,  0.5f,  0.5f, -0.5f,  0.5f, -0.5f,
    /* right */
     0.5f, -0.5f,  0.5f,  0.5f, -0.5f, -0.5f,  0.5f,  0.5f, -0.5f,
     0.5f, -0.5f,  0.5f,  0.5f,  0.5f, -0.5f,  0.5f,  0.5f,  0.5f,
    /* top */
    -0.5f,  0.5f,  0.5f,  0.5f,  0.5f,  0.5f,  0.5f,  0.5f, -0.5f,
    -0.5f,  0.5f,  0.5f,  0.5f,  0.5f, -0.5f, -0.5f,  0.5f, -0.5f,
    /* bottom */
    -0.5f, -0.5f, -0.5f,  0.5f, -0.5f, -0.5f,  0.5f, -0.5f,  0.5f,
    -0.5f, -0.5f, -0.5f,  0.5f, -0.5f,  0.5f, -0.5f, -0.5f,  0.5f
};

static float cube_colors[SDL_arraysize(cube_positions)];

static const float quad_positions[] = {
    -1.0f, -1.0f,  1.0f, -1.0f, -1.0f, 1.0f,  1.0f, 1.0f
};

/* --- shaders ----------------------------------------------------------------- */

static const char *cube_vert_src =
    "attribute vec4 av4position;\n"
    "attribute vec3 av3color;\n"
    "uniform mat4 mvp;\n"
    "varying vec3 vv3color;\n"
    "void main() {\n"
    "    vv3color = av3color;\n"
    "    gl_Position = mvp * av4position;\n"
    "}\n";

static const char *cube_frag_src =
    "precision mediump float;\n"
    "varying vec3 vv3color;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(vv3color, 1.0);\n"
    "}\n";

static const char *quad_vert_src =
    "attribute vec2 av2position;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = av2position * 0.5 + 0.5;\n"
    "    gl_Position = vec4(av2position, 0.0, 1.0);\n"
    "}\n";

/* Heavy fragment shader. ITERS is baked per stage variant via a #define
 * prepended at compile time — all variants are precompiled at startup.
 * u_params = (time, layer_phase, 0, 0). Output is scaled small because the
 * layers accumulate ADDITIVELY (GL_ONE, GL_ONE): the pattern visibly brightens
 * and churns as layers stack up — the escalation is visible on the panel. */
static const char *quad_frag_src_tmpl =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform vec4 u_params;\n"
    "void main() {\n"
    "    vec2 p = v_uv * 8.0 + vec2(u_params.y * 1.7, -u_params.y * 0.9);\n"
    "    float t = u_params.x;\n"
    "    float acc = 0.0;\n"
    "    for (int i = 0; i < ITERS; i++) {\n"
    "        float fi = float(i);\n"
    "        p += vec2(sin(p.y + t + fi * 0.37), cos(p.x - t * 1.3 + fi * 0.71)) * 0.35;\n"
    "        acc += sin(p.x * 1.7 + fi) * cos(p.y * 1.3 - fi);\n"
    "    }\n"
    "    float v = 0.5 + 0.5 * sin(acc);\n"
    /* weights calibrated for panel visibility (device window 2026-07-16): the
     * original 0.040/0.024/0.052 was invisible on the webcam evidence — the
     * plasma churn must be SEEN escalating (acceptance is visual) */
    "    gl_FragColor = vec4(v * 0.14, v * 0.09, v * 0.18, 1.0);\n"
    "}\n";

static const char *particle_vert_src =
    "attribute vec3 av3particle;\n" /* x, y, speed */
    "uniform vec4 u_misc;\n"        /* point_size, 0, 0, 0 */
    "varying float v_speed;\n"
    "void main() {\n"
    "    v_speed = av3particle.z;\n"
    "    gl_PointSize = u_misc.x;\n"
    "    gl_Position = vec4(av3particle.xy, 0.0, 1.0);\n"
    "}\n";

static const char *particle_frag_src =
    "precision mediump float;\n"
    "varying float v_speed;\n"
    "void main() {\n"
    "    float s = clamp(v_speed * 2.0, 0.0, 1.0);\n"
    "    gl_FragColor = vec4(0.2 + 0.8 * s, 0.9 - 0.5 * s, 0.3, 1.0);\n"
    "}\n";

/* --- GL objects --------------------------------------------------------------- */

typedef struct
{
    GLuint program;
    GLint attr_position;
    GLint attr_color;
    GLint unif_mvp;
} CubeShader;

typedef struct
{
    GLuint program;
    GLint attr_position;
    GLint unif_params;
} QuadShader;

typedef struct
{
    GLuint program;
    GLint attr_particle;
    GLint unif_misc;
} ParticleShader;

static CubeShader cube_shader;
static QuadShader quad_shaders[PF_MAX_STAGES]; /* one precompiled variant per stage */
static ParticleShader particle_shader;
static GLuint cube_pos_vbo, cube_col_vbo, quad_vbo, particle_vbo;

/* --- particle sim -------------------------------------------------------------- */

typedef struct
{
    float *px, *py, *vx, *vy;
    float *upload; /* interleaved x,y,speed for the VBO */
    int alive;
    int cap;
} ParticleSim;

static ParticleSim sim;

static bool sim_init(int cap)
{
    sim.px = (float *)SDL_calloc(cap, sizeof(float));
    sim.py = (float *)SDL_calloc(cap, sizeof(float));
    sim.vx = (float *)SDL_calloc(cap, sizeof(float));
    sim.vy = (float *)SDL_calloc(cap, sizeof(float));
    sim.upload = (float *)SDL_calloc(cap, 3 * sizeof(float));
    sim.alive = 0;
    sim.cap = cap;
    return sim.px && sim.py && sim.vx && sim.vy && sim.upload;
}

static void sim_free(void)
{
    SDL_free(sim.px);
    SDL_free(sim.py);
    SDL_free(sim.vx);
    SDL_free(sim.vy);
    SDL_free(sim.upload);
    SDL_zero(sim);
}

static void sim_grow(int target)
{
    if (target > sim.cap) {
        target = sim.cap;
    }
    while (sim.alive < target) {
        int i = sim.alive++;
        sim.px[i] = frand() * 1.8f - 0.9f;
        sim.py[i] = frand() * 1.8f - 0.9f;
        sim.vx[i] = (frand() - 0.5f) * 0.6f;
        sim.vy[i] = (frand() - 0.5f) * 0.6f;
    }
}

/* One Euler step: gravity + wall bounce + a CAPPED sliding-window pairwise
 * repulsion (each particle vs its next K array neighbors → O(n*K), bounded).
 * This is the deliberate CPU axis; it feeds the upload buffer, so the work is
 * real render input, not busywork. */
static void sim_step(float dt)
{
    const int K = 16;
    const float r2 = 0.0025f;
    int i, j, k;

    if (dt > 0.05f) {
        dt = 0.05f; /* clamp so a 2 fps frame doesn't explode the integration */
    }
    for (i = 0; i < sim.alive; i++) {
        for (k = 1; k <= K; k++) {
            j = i + k;
            if (j >= sim.alive) {
                break;
            }
            {
                float dx = sim.px[j] - sim.px[i];
                float dy = sim.py[j] - sim.py[i];
                float d2 = dx * dx + dy * dy + 1e-6f;
                if (d2 < r2) {
                    float f = (r2 - d2) * 40.0f * dt;
                    sim.vx[i] -= dx * f;
                    sim.vy[i] -= dy * f;
                    sim.vx[j] += dx * f;
                    sim.vy[j] += dy * f;
                }
            }
        }
        sim.vy[i] -= 0.25f * dt; /* gravity */
        sim.px[i] += sim.vx[i] * dt;
        sim.py[i] += sim.vy[i] * dt;
        if (sim.px[i] < -1.0f) { sim.px[i] = -1.0f; sim.vx[i] = -sim.vx[i] * 0.9f; }
        if (sim.px[i] >  1.0f) { sim.px[i] =  1.0f; sim.vx[i] = -sim.vx[i] * 0.9f; }
        if (sim.py[i] < -1.0f) { sim.py[i] = -1.0f; sim.vy[i] = -sim.vy[i] * 0.9f; }
        if (sim.py[i] >  1.0f) { sim.py[i] =  1.0f; sim.vy[i] = -sim.vy[i] * 0.9f; }
        {
            float sp = (float)SDL_sqrt(sim.vx[i] * sim.vx[i] + sim.vy[i] * sim.vy[i]);
            sim.upload[i * 3 + 0] = sim.px[i];
            sim.upload[i * 3 + 1] = sim.py[i];
            sim.upload[i * 3 + 2] = sp;
        }
    }
}

/* --- GL helpers ------------------------------------------------------------------ */

static bool LoadContext(GLES2_Context *data)
{
#ifdef SDL_VIDEO_DRIVER_UIKIT
#define __SDL_NOGETPROCADDR__
#elif defined(SDL_VIDEO_DRIVER_ANDROID)
#define __SDL_NOGETPROCADDR__
#endif

#if defined __SDL_NOGETPROCADDR__
#define SDL_PROC(ret, func, params) data->func = func;
#else
#define SDL_PROC(ret, func, params)                                                            \
    do {                                                                                       \
        data->func = (ret (APIENTRY *) params)SDL_GL_GetProcAddress(#func);                    \
        if (!data->func) {                                                                     \
            return SDL_SetError("Couldn't load GLES2 function %s: %s", #func, SDL_GetError()); \
        }                                                                                      \
    } while (0);
#endif /* __SDL_NOGETPROCADDR__ */

#include "../src/render/opengles2/SDL_gles2funcs.h"
#undef SDL_PROC
    return true;
}

static GLuint compile_shader(const char *src, GLenum type)
{
    GLuint shader = ctx.glCreateShader(type);
    GLint status;
    ctx.glShaderSource(shader, 1, &src, NULL);
    ctx.glCompileShader(shader);
    ctx.glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        char log[1024];
        GLsizei len = 0;
        ctx.glGetShaderInfoLog(shader, sizeof(log) - 1, &len, log);
        log[len] = '\0';
        SDL_Log("shader compile failed: %s", log);
        ctx.glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint link_program(GLuint vert, GLuint frag)
{
    GLuint program = ctx.glCreateProgram();
    GLint status;
    ctx.glAttachShader(program, vert);
    ctx.glAttachShader(program, frag);
    ctx.glLinkProgram(program);
    ctx.glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        char log[1024];
        GLsizei len = 0;
        ctx.glGetProgramInfoLog(program, sizeof(log) - 1, &len, log);
        log[len] = '\0';
        SDL_Log("program link failed: %s", log);
        ctx.glDeleteProgram(program);
        return 0;
    }
    return program;
}

static GLuint build_program(const char *vsrc, const char *fsrc)
{
    GLuint vert = compile_shader(vsrc, GL_VERTEX_SHADER);
    GLuint frag = vert ? compile_shader(fsrc, GL_FRAGMENT_SHADER) : 0;
    GLuint program = (vert && frag) ? link_program(vert, frag) : 0;
    if (vert) {
        ctx.glDeleteShader(vert);
    }
    if (frag) {
        ctx.glDeleteShader(frag);
    }
    return program;
}

/* --- stage parameters -------------------------------------------------------------- */

static int stage_cubes(int stage)
{
    double n = PF_CUBES_BASE;
    int i;
    for (i = 0; i < stage; i++) {
        n *= PF_CUBES_GROWTH;
    }
    if (n > PF_CUBES_CAP) {
        n = PF_CUBES_CAP;
    }
    return (int)n;
}

/* Per-stage fragment-load schedule (fill cost ≈ layers × iters), CALIBRATED on
 * the A133/GE8300 (2026-07-16 device window, run 1 evidence on bead
 * tsp-147u.8.5): measured fps ≈ min(~60, K/cost) with K ≈ 300, and the display
 * path paces ~60 even at swap-interval 0 — so cost growing ~1.55x/stage yields
 * the owner's target curve on THIS device: ~60, 60, 37, 25, 17, 11, 7.5, 4.8,
 * 3, ~2 fps. (The original linear schedule front-loaded a 7.5x jump at stage 1
 * — 60→15 fps — and bottomed at 0.6 fps; run-1 data drove this retune.)
 * Stages past 9 (a future strong device runs --stages >10) keep escalating at
 * ~1.55x cost/stage via the extrapolation below, capped by PF_LAYERS_CAP. */
static const int stage_fill_table[10][2] = {
    /* layers, iters */
    { 1, 3 }, { 1, 5 }, { 2, 4 }, { 2, 6 }, { 3, 6 },
    { 4, 7 }, { 5, 8 }, { 7, 9 }, { 9, 11 }, { 12, 13 },
};

static int stage_layers(int stage)
{
    double n = 12.0;
    int i;
    if (stage < 10) {
        return stage_fill_table[stage][0];
    }
    for (i = 10; i <= stage; i++) {
        n *= 1.35;
    }
    return (n > PF_LAYERS_CAP) ? PF_LAYERS_CAP : (int)n;
}

static int stage_iters(int stage)
{
    double n = 13.0;
    int i;
    if (stage < 10) {
        return stage_fill_table[stage][1];
    }
    for (i = 10; i <= stage; i++) {
        n *= 1.15;
    }
    return (n > 64) ? 64 : (int)n;
}

static int stage_particles(int stage)
{
    long n = (long)PF_PARTICLES_BASE * (stage + 1) * (stage + 1);
    return (n > PF_PARTICLES_CAP) ? PF_PARTICLES_CAP : (int)n;
}

/* --- teardown ---------------------------------------------------------------------- */

static void quit(int rc)
{
    sim_free();
    if (gl_context) {
        SDL_GL_DestroyContext(gl_context);
        gl_context = NULL;
    }
    SDLTest_CommonQuit(state);
    exit(rc);
}

/* Post-run hard exit (bead tsp-mc9m.41.935; revised per PR#18 review rounds 1-2).
 *
 * After the benchmark loop finishes and the summary is printed, the normal
 * quit() teardown path (SDL_GL_DestroyContext -> SDLTest_CommonQuit's
 * SDL_DestroyWindow, which unbinds the GL context via
 * SDL_GL_MakeCurrent(window, NULL) since it was never explicitly unbound
 * while current -> SDL_Quit -> SUNXIFB_VideoQuit's VT_ACTIVATE ioctls) was
 * observed to hang indefinitely on tsp-base's GE8300 sunxifb/NULL_WSEGL
 * path: the process never returns to the shell and needs the harness's 35s
 * outer timeout to SIGKILL it, even though the summary had already printed
 * at 30s. The benchmark result is fully valid at that point (every summary
 * field was flushed to stdout already); only the subsequent GL/EGL/VT
 * teardown is suspect, and there is no DUT available here to bisect which
 * specific call (context destroy of a context that was still current, EGL
 * surface destroy, or the VT_ACTIVATE pair) is the one that blocks.
 *
 * This workaround is sunxifb-SPECIFIC: only that driver's teardown is known
 * to hang, so the caller gates this against SDL_GetCurrentVideoDriver() and
 * only reaches here when sunxifb is active -- every other GLES2 backend
 * (x11, wayland, kmsdrm, ...) keeps going through the normal quit() below,
 * unaffected. Skip the GL/EGL/VT teardown -- but NOT unconditionally:
 * SUNXIFB_VideoInit disables the terminal cursor ("setterm -cursor off")
 * and only SUNXIFB_VideoQuit re-enables it, which normal teardown would
 * reach only AFTER the suspect GL/EGL/window path. Restore the cursor here,
 * first and directly (the same call SUNXIFB_VideoQuit makes -- cheap, and
 * touches neither GL/EGL nor the VT_ACTIVATE ioctls), so it always runs
 * before the hard exit regardless of what happens further down the normal
 * teardown chain. Use portable C99 _Exit() (stdlib.h, already included)
 * rather than POSIX _exit()/<unistd.h> so this test keeps building on
 * non-POSIX platforms; both skip atexit/SDL cleanup identically and cannot
 * hang on the GL/EGL/VT calls above. DUT-verify-pending: confirm on
 * tsp-base that `SDL_VIDEODRIVER=sunxifb pf-gfxbench` now returns (exit
 * matching rc) within a couple seconds of the pf_gfxbench_status line, AND
 * that the shell's cursor is back on afterward. */
static void quit_after_run(int rc)
{
    fflush(stdout);
    system("setterm -cursor on");
    _Exit(rc);
}

/* --- main --------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    int i;
    int done = 0;
    int w = 0, h = 0;
    int vsync_actual = -1;
    const char *status = "complete";
    int exit_code = 0;

    /* per-stage accounting */
    long stage_frames[PF_MAX_STAGES];
    double stage_time[PF_MAX_STAGES];
    int last_stage_entered = 0;

    SDL_zeroa(stage_frames);
    SDL_zeroa(stage_time);

    state = SDLTest_CommonCreateState(argv, SDL_INIT_VIDEO);
    if (!state) {
        return 2;
    }

    for (i = 1; i < argc;) {
        int consumed = SDLTest_CommonArg(state, i);
        if (consumed == 0) {
            if (SDL_strcasecmp(argv[i], "--duration") == 0 && argv[i + 1]) {
                opt_duration_s = SDL_atof(argv[++i]);
                consumed = 2;
                i--; /* rewind: consumed counts from the flag */
            } else if (SDL_strcasecmp(argv[i], "--stages") == 0 && argv[i + 1]) {
                opt_stages = SDL_atoi(argv[++i]);
                consumed = 2;
                i--;
            } else if (SDL_strcasecmp(argv[i], "--report-interval") == 0 && argv[i + 1]) {
                opt_report_s = SDL_atof(argv[++i]);
                consumed = 2;
                i--;
            } else if (SDL_strcasecmp(argv[i], "--watchdog") == 0 && argv[i + 1]) {
                opt_watchdog_s = SDL_atof(argv[++i]);
                consumed = 2;
                i--;
            } else if (SDL_strcasecmp(argv[i], "--vsync") == 0) {
                opt_vsync = 1;
                consumed = 1;
            } else {
                consumed = -1;
            }
        }
        if (consumed < 0) {
            static const char *options[] = {
                "[--duration seconds]", "[--stages n]", "[--report-interval seconds]",
                "[--watchdog seconds]", "[--vsync]", NULL
            };
            SDLTest_CommonLogUsage(state, argv[0], options);
            quit(2);
        }
        i += consumed;
    }

    if (opt_stages < 2) {
        opt_stages = 2;
    }
    if (opt_stages > PF_MAX_STAGES) {
        opt_stages = PF_MAX_STAGES;
    }
    if (opt_duration_s < 2.0) {
        opt_duration_s = 2.0;
    }

    state->window_flags |= SDL_WINDOW_OPENGL;
    state->gl_red_size = 5;
    state->gl_green_size = 5;
    state->gl_blue_size = 5;
    state->gl_depth_size = 16;
    state->gl_major_version = 2;
    state->gl_minor_version = 0;
    state->gl_profile_mask = SDL_GL_CONTEXT_PROFILE_ES;

    if (!SDLTest_CommonInit(state)) {
        quit(2);
    }

    gl_context = SDL_GL_CreateContext(state->windows[0]);
    if (!gl_context) {
        SDL_Log("SDL_GL_CreateContext(): %s", SDL_GetError());
        quit(2);
    }
    if (!LoadContext(&ctx)) {
        SDL_Log("Could not load GLES2 functions");
        quit(2);
    }

    /* Uncapped by default: a vsync cap clamps the top of the curve and hides
     * the cross-device differentiation the test exists to produce. --vsync
     * keeps the cap; the ACTUAL interval is reported either way. */
    SDL_GL_SetSwapInterval(opt_vsync);
    if (!SDL_GL_GetSwapInterval(&vsync_actual)) {
        vsync_actual = -1;
    }

    SDL_GetWindowSizeInPixels(state->windows[0], &w, &h);
    ctx.glViewport(0, 0, w, h);

    printf("pf_gfxbench_version=1\n");
    printf("config duration_s=%.1f stages=%d stage_s=%.2f report_s=%.2f vsync_req=%d vsync_actual=%d width=%d height=%d\n",
           opt_duration_s, opt_stages, opt_duration_s / opt_stages, opt_report_s,
           opt_vsync, vsync_actual, w, h);
    printf("gl_vendor=%s\n", (const char *)ctx.glGetString(GL_VENDOR));
    printf("gl_renderer=%s\n", (const char *)ctx.glGetString(GL_RENDERER));
    printf("gl_version=%s\n", (const char *)ctx.glGetString(GL_VERSION));
    fflush(stdout);

    /* --- build GL resources ------------------------------------------------ */

    cube_shader.program = build_program(cube_vert_src, cube_frag_src);
    if (!cube_shader.program) {
        quit(2);
    }
    cube_shader.attr_position = ctx.glGetAttribLocation(cube_shader.program, "av4position");
    cube_shader.attr_color = ctx.glGetAttribLocation(cube_shader.program, "av3color");
    cube_shader.unif_mvp = ctx.glGetUniformLocation(cube_shader.program, "mvp");

    /* Precompile EVERY stage's heavy-fragment variant up front (compile jank
     * must not pollute mid-run FPS samples). */
    for (i = 0; i < opt_stages; i++) {
        char fsrc[2048];
        SDL_snprintf(fsrc, sizeof(fsrc), "#define ITERS %d\n%s", stage_iters(i), quad_frag_src_tmpl);
        quad_shaders[i].program = build_program(quad_vert_src, fsrc);
        if (!quad_shaders[i].program) {
            quit(2);
        }
        quad_shaders[i].attr_position = ctx.glGetAttribLocation(quad_shaders[i].program, "av2position");
        quad_shaders[i].unif_params = ctx.glGetUniformLocation(quad_shaders[i].program, "u_params");
    }

    particle_shader.program = build_program(particle_vert_src, particle_frag_src);
    if (!particle_shader.program) {
        quit(2);
    }
    particle_shader.attr_particle = ctx.glGetAttribLocation(particle_shader.program, "av3particle");
    particle_shader.unif_misc = ctx.glGetUniformLocation(particle_shader.program, "u_misc");

    /* cube colors: deterministic pseudo-random face tints */
    for (i = 0; i < (int)SDL_arraysize(cube_colors); i++) {
        cube_colors[i] = 0.25f + 0.75f * frand();
    }

    ctx.glGenBuffers(1, &cube_pos_vbo);
    ctx.glBindBuffer(GL_ARRAY_BUFFER, cube_pos_vbo);
    ctx.glBufferData(GL_ARRAY_BUFFER, sizeof(cube_positions), cube_positions, GL_STATIC_DRAW);
    ctx.glGenBuffers(1, &cube_col_vbo);
    ctx.glBindBuffer(GL_ARRAY_BUFFER, cube_col_vbo);
    ctx.glBufferData(GL_ARRAY_BUFFER, sizeof(cube_colors), cube_colors, GL_STATIC_DRAW);
    ctx.glGenBuffers(1, &quad_vbo);
    ctx.glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    ctx.glBufferData(GL_ARRAY_BUFFER, sizeof(quad_positions), quad_positions, GL_STATIC_DRAW);
    ctx.glGenBuffers(1, &particle_vbo);
    ctx.glBindBuffer(GL_ARRAY_BUFFER, particle_vbo);
    ctx.glBufferData(GL_ARRAY_BUFFER, PF_PARTICLES_CAP * 3 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    ctx.glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (!sim_init(PF_PARTICLES_CAP)) {
        SDL_Log("Out of memory for particle sim");
        quit(2);
    }

    ctx.glClearColor(0.02f, 0.02f, 0.05f, 1.0f);

    /* --- run ----------------------------------------------------------------- */
    {
        const double stage_s = opt_duration_s / opt_stages;
        const double deadline_s = opt_duration_s + PF_DEF_DEADLINE_SLOP_S;
        Uint64 t0;
        double warmup_until = PF_DEF_WARMUP_S;
        bool warming = true;
        double now_s = 0.0, prev_s = 0.0;
        double window_start = 0.0;
        long window_frames = 0;
        long total_frames = 0;
        int stage = -1;
        float proj[16];
        double bench_start = 0.0;

        mat_perspective(45.0f, (float)w / (float)h, 0.1f, 100.0f, proj);

        t0 = SDL_GetTicksNS();
        while (!done) {
            SDL_Event event;
            double frame_start;
            int cubes, layers, particles;
            int new_stage;

            while (SDL_PollEvent(&event)) {
                SDLTest_CommonEvent(state, &event, &done);
            }
            if (done) {
                status = "user_abort";
                exit_code = 3;
                break;
            }

            now_s = (double)(SDL_GetTicksNS() - t0) / 1e9;
            frame_start = now_s;

            if (warming) {
                if (now_s >= warmup_until) {
                    /* benchmark clock starts now; warmup frames are excluded */
                    warming = false;
                    bench_start = now_s;
                    window_start = 0.0;
                    window_frames = 0;
                    prev_s = now_s;
                    stage = -1; /* re-fire stage-change detection so stage 0's
                                 * stage_config line prints (it was entered
                                 * during warmup with printing suppressed) */
                }
            }
            {
                double bench_now = warming ? 0.0 : now_s - bench_start;

                if (!warming && bench_now >= opt_duration_s) {
                    break; /* complete */
                }
                if (!warming && now_s - bench_start >= deadline_s) {
                    status = "deadline_abort";
                    exit_code = 3;
                    break;
                }

                new_stage = warming ? 0 : (int)(bench_now / stage_s);
                if (new_stage >= opt_stages) {
                    new_stage = opt_stages - 1;
                }
                if (new_stage != stage) {
                    stage = new_stage;
                    if (stage > last_stage_entered) {
                        last_stage_entered = stage;
                    }
                    sim_grow(stage_particles(stage));
                    if (!warming) {
                        printf("stage_config stage=%d t=%.2f cubes=%d layers=%d iters=%d particles=%d\n",
                               stage, bench_now, stage_cubes(stage), stage_layers(stage),
                               stage_iters(stage), stage_particles(stage));
                        fflush(stdout);
                    }
                }

                cubes = stage_cubes(stage);
                layers = stage_layers(stage);
                particles = sim.alive;

                /* CPU axis: particle integration + interaction (bounded O(n*K)) */
                sim_step((float)(now_s - prev_s));
                prev_s = now_s;

                /* --- render ---------------------------------------------------- */
                ctx.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

                /* 1. cube field — one draw call + one CPU MVP per cube */
                ctx.glEnable(GL_DEPTH_TEST);
                ctx.glEnable(GL_CULL_FACE);
                ctx.glDisable(GL_BLEND);
                ctx.glUseProgram(cube_shader.program);
                ctx.glBindBuffer(GL_ARRAY_BUFFER, cube_pos_vbo);
                ctx.glEnableVertexAttribArray(cube_shader.attr_position);
                ctx.glVertexAttribPointer(cube_shader.attr_position, 3, GL_FLOAT, GL_FALSE, 0, NULL);
                ctx.glBindBuffer(GL_ARRAY_BUFFER, cube_col_vbo);
                ctx.glEnableVertexAttribArray(cube_shader.attr_color);
                ctx.glVertexAttribPointer(cube_shader.attr_color, 3, GL_FLOAT, GL_FALSE, 0, NULL);
                {
                    int g = 1;
                    int c;
                    float angle = (float)(now_s * 60.0);
                    while (g * g < cubes) {
                        g++;
                    }
                    for (c = 0; c < cubes; c++) {
                        float model[16], rot[16], mvp[16];
                        float scale = 3.0f / (float)g;
                        int gx = c % g, gy = c / g;
                        mat_rotate(angle + (float)c * 7.0f, 1.0f, 0.8f, 0.6f, rot);
                        mat_identity(model);
                        model[0] = model[5] = model[10] = scale * 0.5f;
                        mat_multiply(model, rot, model);
                        model[12] = ((float)gx - (float)(g - 1) / 2.0f) * scale;
                        model[13] = ((float)gy - (float)(g - 1) / 2.0f) * scale;
                        model[14] = -5.0f;
                        mat_multiply(mvp, proj, model);
                        ctx.glUniformMatrix4fv(cube_shader.unif_mvp, 1, GL_FALSE, mvp);
                        ctx.glDrawArrays(GL_TRIANGLES, 0, 36);
                    }
                }
                ctx.glDisableVertexAttribArray(cube_shader.attr_position);
                ctx.glDisableVertexAttribArray(cube_shader.attr_color);

                /* 2. fullscreen overdraw layers — additive blending defeats TBDR
                 *    hidden-surface removal, so every layer shades every pixel */
                ctx.glDisable(GL_DEPTH_TEST);
                ctx.glDisable(GL_CULL_FACE);
                ctx.glEnable(GL_BLEND);
                ctx.glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
                ctx.glUseProgram(quad_shaders[stage].program);
                ctx.glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
                ctx.glEnableVertexAttribArray(quad_shaders[stage].attr_position);
                ctx.glVertexAttribPointer(quad_shaders[stage].attr_position, 2, GL_FLOAT, GL_FALSE, 0, NULL);
                {
                    int l;
                    for (l = 0; l < layers; l++) {
                        ctx.glUniform4f(quad_shaders[stage].unif_params,
                                        (float)now_s, (float)l, 0.0f, 0.0f);
                        ctx.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    }
                }
                ctx.glDisableVertexAttribArray(quad_shaders[stage].attr_position);

                /* 3. particles — per-frame client-buffer upload (GPU transfer axis) */
                if (particles > 0) {
                    ctx.glDisable(GL_BLEND);
                    ctx.glUseProgram(particle_shader.program);
                    ctx.glBindBuffer(GL_ARRAY_BUFFER, particle_vbo);
                    ctx.glBufferData(GL_ARRAY_BUFFER, PF_PARTICLES_CAP * 3 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
                    ctx.glBufferSubData(GL_ARRAY_BUFFER, 0, particles * 3 * sizeof(float), sim.upload);
                    ctx.glEnableVertexAttribArray(particle_shader.attr_particle);
                    ctx.glVertexAttribPointer(particle_shader.attr_particle, 3, GL_FLOAT, GL_FALSE, 0, NULL);
                    ctx.glUniform4f(particle_shader.unif_misc, 3.0f, 0.0f, 0.0f, 0.0f);
                    ctx.glDrawArrays(GL_POINTS, 0, particles);
                    ctx.glDisableVertexAttribArray(particle_shader.attr_particle);
                }
                ctx.glBindBuffer(GL_ARRAY_BUFFER, 0);

                SDL_GL_SwapWindow(state->windows[0]);

                now_s = (double)(SDL_GetTicksNS() - t0) / 1e9;

                /* per-frame watchdog: one pathological frame must not wedge the
                 * device — abort GRACEFULLY with a partial summary instead */
                if (now_s - frame_start > opt_watchdog_s) {
                    printf("watchdog frame_s=%.2f stage=%d\n", now_s - frame_start, stage);
                    fflush(stdout);
                    status = "watchdog_abort";
                    exit_code = 3;
                    break;
                }

                if (!warming) {
                    total_frames++;
                    window_frames++;
                    stage_frames[stage]++;
                    stage_time[stage] += now_s - frame_start > 0.0 ? (now_s - frame_start) : 0.0;

                    bench_now = now_s - bench_start;
                    if (bench_now - window_start >= opt_report_s) {
                        double fps = (double)window_frames / (bench_now - window_start);
                        printf("fps_sample t=%.2f stage=%d fps=%.2f\n", bench_now, stage, fps);
                        fflush(stdout);
                        window_start = bench_now;
                        window_frames = 0;
                    }
                }
            }
        }

        /* --- summary ---------------------------------------------------------- */
        {
            double start_fps = 0.0, floor_fps = 0.0, gradient = 0.0;
            double wall_s = warming ? 0.0 : now_s - bench_start;
            int floor_stage = -1;

            if (stage_time[0] > 0.0) {
                start_fps = (double)stage_frames[0] / stage_time[0];
            }
            /* floor = the LAST stage with >= 0.5s of accounted frame time (a
             * partially-entered final stage with one frame would be noise) */
            for (i = last_stage_entered; i >= 0; i--) {
                if (stage_time[i] >= 0.5) {
                    floor_stage = i;
                    break;
                }
            }
            if (floor_stage >= 0) {
                floor_fps = (double)stage_frames[floor_stage] / stage_time[floor_stage];
            }
            if (floor_fps > 0.0) {
                gradient = start_fps / floor_fps;
            }

            for (i = 0; i <= last_stage_entered; i++) {
                if (stage_time[i] > 0.0) {
                    printf("stage_result stage=%d frames=%ld avg_fps=%.2f\n",
                           i, stage_frames[i], (double)stage_frames[i] / stage_time[i]);
                }
            }
            printf("summary start_fps=%.2f floor_fps=%.2f gradient_ratio=%.2f score_frames=%ld frames=%ld wall_s=%.2f stages_completed=%d/%d floor_stage=%d\n",
                   start_fps, floor_fps, gradient, total_frames, total_frames, wall_s,
                   last_stage_entered + 1, opt_stages, floor_stage);
            printf("pf_gfxbench_status=%s\n", status);
            fflush(stdout);
        }
    }

    /* The post-run teardown hang (see quit_after_run() above) is
     * sunxifb-specific -- gate the workaround to that driver only, so every
     * other GLES2 backend keeps its normal, full SDL/GL/EGL teardown. */
    {
        const char *driver = SDL_GetCurrentVideoDriver();
        if (driver && SDL_strcmp(driver, "sunxifb") == 0) {
            quit_after_run(exit_code);
        } else {
            quit(exit_code);
        }
    }
    return 0;
}

#else /* HAVE_OPENGLES2 */

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    fprintf(stderr, "pf_gfxbench: no OpenGL ES 2 support on this system\n");
    printf("pf_gfxbench_status=no_gles2\n");
    return 2;
}

#endif /* HAVE_OPENGLES2 */
