/*
 * tsp-mc9m.41.924.16.12 hermetic test support: the report a fake
 * libdrm/libgbm/libEGL/libGLESv2 (fake-kms.c, one shared object installed
 * under all four sonames) keeps while SDL's real KMSDRM backend drives it.
 *
 * The fake models a 720x1280 portrait DSI panel whose connector publishes a
 * "panel orientation" property chosen by FAKE_KMS_PANEL_ORIENTATION ("Normal",
 * "Left Side Up", "Right Side Up", "Upside Down", or "none" for no property),
 * with the kernel's enum table and property semantics. It enforces the
 * contracts the real stack enforces and counts every violation in `errors`.
 */
#ifndef FAKE_KMS_H
#define FAKE_KMS_H

#define FAKE_KMS_PANEL_W 720
#define FAKE_KMS_PANEL_H 1280
#define FAKE_KMS_BOS_PER_SURFACE 4
#define FAKE_KMS_MAX_SURFACES 16

typedef struct FakeKmsSurfaceReport
{
    int w, h;
    unsigned format;      // GBM fourcc passed to gbm_surface_create
    unsigned flags;       // gbm_surface_create flags
    int alive;
    int swaps;            // eglSwapBuffers on the EGL surface made from it
    int locked_now;
    int locked_max;
    int clears;           // glClear with framebuffer 0 bound while drawing into it
} FakeKmsSurfaceReport;

typedef struct FakeKmsDraw
{
    void *ctx;            // EGLContext current at glDrawArrays
    int target_surface;   // gbm surface index of the current draw surface
    int source_surface;   // gbm surface index of the sampled buffer
    int source_w, source_h;
    int source_frame;     // the source surface's swap count when it was swapped
    int source_locked;    // the sampled buffer was locked (not handed back)
    int viewport[4];
    float pos[8];
    float uv[8];
} FakeKmsDraw;

typedef struct FakeKmsReport
{
    int errors;
    char first_error[512];

    int surfaces_created;
    FakeKmsSurfaceReport surfaces[FAKE_KMS_MAX_SURFACES];

    int contexts_created;
    int contexts_alive;
    int egl_surfaces_alive;
    // FAKE_EGL_FAIL_MAKECURRENT_SURFACE=N: eglMakeCurrent failures injected when a
    // context is bound to an EGL surface made from gbm surface N (once).
    int injected_makecurrent_failures;
    // vkEnumerateInstanceExtensionProperties calls (the fake libvulkan.so.1).
    int vulkan_enumerations;
    int images_created;
    int images_alive;

    int draws;
    FakeKmsDraw last_draw;

    /* SDL_Renderer GLES2 draws are distinct from the imported-image rotate
       pass above. These fields make the pre-rotation viewport/projection and
       clip/scissor contract observable without modelling rasterization. */
    int renderer_draws;
    int renderer_target_surface;
    int renderer_viewport[4];
    int renderer_scissor[4];
    float renderer_projection[16];
    int renderer_texture_target_draws;
    int renderer_texture_target_viewport[4];
    float renderer_texture_target_projection[16];
    int readpixels_calls;
    int readpixels_rect[4];

    /* tsp-mc9m.41.924.16.13.3, SDL_KMSDRM_ROTATE_EXPERIMENT: glCopyTexSubImage2D
       calls (the copy texture then samples as the read framebuffer's image), and
       eglGetProcAddress lookups of the entry points only an experiment uses
       (framebuffer objects, the copy, glTexImage2D). The ordinary pass makes none. */
    int copies;
    int experiment_proc_requests;

    int syncs_alive;
    int cross_context_waits;   // eglWaitSyncKHR on a fence made in another context
    int client_waits;          // eglClientWaitSyncKHR calls

    // Built with -DFAKE_KMS_OMIT_GL_LINK_PROGRAM: glLinkProgram is neither exported
    // nor resolvable, and eglGetProcAddress("glLinkProgram") is counted here.
    int omitted_proc_requests;

    // The last buffer put on the panel (SetCrtc, page flip, or atomic FB_ID).
    int scanouts;
    int scanout_surface;       // gbm surface index, -1 for a non-surface FB
    int scanout_w, scanout_h;
    int plane_src_w, plane_src_h;   // atomic SRC_W/SRC_H >> 16 of the last commit
    int plane_crtc_w, plane_crtc_h;
    int atomic_commits;
    // FAKE_KMS_FLIP_MS > 0 (tsp-mc9m.41.924.16.13): each flip completes that
    // long after its commit, and its OUT_FENCE is a timerfd that polls
    // readable at that moment, as a sync_file does.
    int flip_fences;           // OUT_FENCE fds handed out for a pending flip
    int busy_commits;          // nonblocking commits made while a flip was pending (-EBUSY)
    int blocking_stalls;       // blocking commits that had to wait for a pending flip

    // EGL state after the last call.
    void *current_ctx;
    int current_draw_surface;  // gbm surface index, -1 for none
} FakeKmsReport;

typedef const FakeKmsReport *(*FakeKmsGetReportFn)(void);

#endif
