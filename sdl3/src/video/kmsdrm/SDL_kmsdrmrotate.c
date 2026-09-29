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
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_KMSDRM

/* Include this first, as some system headers may pull in EGL headers that
 * define EGL types as native types for other enabled platforms, which can
 * result in type-mismatch warnings when building with LTO.
 */
#include "../SDL_egl_c.h"

#include <SDL3/SDL_opengles2.h>

#include "SDL_kmsdrmdyn.h"
#include "SDL_kmsdrmvideo.h"
#include "SDL_kmsdrmrotate.h"
#include "SDL_kmsdrmrotategl.h"
#include "SDL_kmsdrmtiming.h"

#include <errno.h>
#include <string.h>

#ifndef EGL_NATIVE_PIXMAP_KHR
#define EGL_NATIVE_PIXMAP_KHR 0x30B0
#endif

/* Mesa's GBM surfaces cycle through at most four buffers; the cache is keyed
   by buffer and lives exactly as long as the application's GBM surface. */
#define KMSDRM_ROTATE_MAX_IMAGES 8

typedef struct KMSDRM_RotateImage
{
    struct gbm_bo *bo;
    EGLImageKHR image;
    GLuint texture;
} KMSDRM_RotateImage;

struct KMSDRM_Rotate
{
    int rotation;
    int pw, ph;

    EGLContext context;
    KMSDRM_RotateGL gl;
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
    bool have_fences;

    KMSDRM_RotateImage images[KMSDRM_ROTATE_MAX_IMAGES];
    int num_images;

    // The application buffer the current present read, and its rotate fence.
    struct gbm_bo *frame_bo;
    EGLSyncKHR frame_fence;
    // The previous present's; returned to the application when its fence signals.
    struct gbm_bo *pending_bo;
    EGLSyncKHR pending_fence;
};

static KMSDRM_RotateGLProc KMSDRM_Rotate_GetProc(void *userdata, const char *name)
{
    return (KMSDRM_RotateGLProc)SDL_EGL_GetProcAddressInternal((SDL_VideoDevice *)userdata, name);
}

// A plain GLES2 window config for the panel-native ARGB8888 present surface.
static EGLConfig KMSDRM_Rotate_ChooseConfig(SDL_VideoDevice *_this)
{
    static const EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig configs[64];
    EGLint count = 0;
    EGLint i;

    if (!_this->egl_data->eglChooseConfig(_this->egl_data->egl_display, attribs,
                                          configs, (EGLint)SDL_arraysize(configs), &count)) {
        return NULL;
    }

    // eglChooseConfig sorts smaller depth/stencil/sample buffers first.
    for (i = 0; i < count; ++i) {
        EGLint visual_id = 0;
        if (_this->egl_data->eglGetConfigAttrib(_this->egl_data->egl_display, configs[i],
                                                EGL_NATIVE_VISUAL_ID, &visual_id) &&
            (uint32_t)visual_id == GBM_FORMAT_ARGB8888) {
            return configs[i];
        }
    }
    return NULL;
}

static void KMSDRM_Rotate_WaitFence(SDL_VideoDevice *_this, EGLSyncKHR fence)
{
    if (fence != EGL_NO_SYNC_KHR) {
        _this->egl_data->eglClientWaitSyncKHR(_this->egl_data->egl_display, fence, 0, EGL_FOREVER_KHR);
        _this->egl_data->eglDestroySyncKHR(_this->egl_data->egl_display, fence);
    }
}

void KMSDRM_Rotate_Destroy(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_WindowData *windata = window->internal;
    struct KMSDRM_Rotate *rot = windata->rotate;
    SDL_EGL_VideoData *egl = _this->egl_data;
    int i;

    if (!rot) {
        return;
    }

    if (egl) {
        egl->eglBindAPI(EGL_OPENGL_ES_API);
        egl->eglMakeCurrent(egl->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    // Hand back the application buffers the last passes read.
    if (rot->frame_bo) {
        if (egl) {
            KMSDRM_Rotate_WaitFence(_this, rot->frame_fence);
        }
        KMSDRM_gbm_surface_release_buffer(windata->gs, rot->frame_bo);
    }
    if (rot->pending_bo) {
        if (egl) {
            KMSDRM_Rotate_WaitFence(_this, rot->pending_fence);
        }
        KMSDRM_gbm_surface_release_buffer(windata->gs, rot->pending_bo);
    }

    /* No GL call here: the program and textures die with the context. That
       also keeps teardown safe after a partial Create, where the GL table
       (KMSDRM_RotateGL_Load is all-or-nothing) may be empty and no image
       exists yet. */
    if (egl) {
        for (i = 0; i < rot->num_images; ++i) {
            if (rot->images[i].image != EGL_NO_IMAGE_KHR && rot->eglDestroyImageKHR) {
                rot->eglDestroyImageKHR(egl->egl_display, rot->images[i].image);
            }
        }
        if (windata->present_egl_surface != EGL_NO_SURFACE) {
            egl->eglDestroySurface(egl->egl_display, windata->present_egl_surface);
        }
        if (rot->context != EGL_NO_CONTEXT) {
            egl->eglDestroyContext(egl->egl_display, rot->context);
        }
    }
    windata->present_egl_surface = EGL_NO_SURFACE;

    if (windata->present_gs) {
        KMSDRM_gbm_surface_destroy(windata->present_gs);
        windata->present_gs = NULL;
    }

    SDL_free(rot);
    windata->rotate = NULL;
}

bool KMSDRM_Rotate_Create(SDL_VideoDevice *_this, SDL_Window *window, int rotation, int pw, int ph)
{
    static const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    SDL_VideoData *viddata = _this->internal;
    SDL_WindowData *windata = window->internal;
    SDL_EGL_VideoData *egl = _this->egl_data;
    const uint32_t surface_fmt = GBM_FORMAT_ARGB8888;
    struct KMSDRM_Rotate *rot;
    EGLConfig config;

    if (!egl) {
        return SDL_SetError("KMSDRM rotated present: EGL is not initialized");
    }
    if (!KMSDRM_RotationIsValid(rotation) || rotation == 0) {
        return SDL_SetError("KMSDRM rotated present: invalid rotation %d", rotation);
    }
    if (!SDL_EGL_HasExtension(_this, SDL_EGL_DISPLAY_EXTENSION, "EGL_KHR_image_pixmap")) {
        return SDL_SetError("KMSDRM rotated present needs EGL_KHR_image_pixmap");
    }

    rot = (struct KMSDRM_Rotate *)SDL_calloc(1, sizeof(*rot));
    if (!rot) {
        return false;
    }
    rot->rotation = rotation;
    rot->pw = pw;
    rot->ph = ph;
    rot->context = EGL_NO_CONTEXT;
    windata->rotate = rot;
    windata->present_gs = NULL;
    windata->present_egl_surface = EGL_NO_SURFACE;

    rot->eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)egl->eglGetProcAddress("eglCreateImageKHR");
    rot->eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)egl->eglGetProcAddress("eglDestroyImageKHR");
    rot->glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)SDL_EGL_GetProcAddressInternal(_this, "glEGLImageTargetTexture2DOES");
    if (!rot->eglCreateImageKHR || !rot->eglDestroyImageKHR || !rot->glEGLImageTargetTexture2DOES) {
        SDL_SetError("KMSDRM rotated present: EGLImage entry points are missing");
        goto fail;
    }
    if (!KMSDRM_RotateGL_Load(&rot->gl, KMSDRM_Rotate_GetProc, _this)) {
        SDL_SetError("KMSDRM rotated present: GLES2 entry points are missing");
        goto fail;
    }
    rot->have_fences = egl->eglCreateSyncKHR && egl->eglDestroySyncKHR &&
                       egl->eglClientWaitSyncKHR && egl->eglWaitSyncKHR &&
                       SDL_EGL_HasExtension(_this, SDL_EGL_DISPLAY_EXTENSION, "EGL_KHR_fence_sync") &&
                       SDL_EGL_HasExtension(_this, SDL_EGL_DISPLAY_EXTENSION, "EGL_KHR_wait_sync");

    config = KMSDRM_Rotate_ChooseConfig(_this);
    if (!config) {
        SDL_SetError("KMSDRM rotated present: no ARGB8888 GLES2 window config");
        goto fail;
    }

    // The panel-native scanout surface: the same kind the unrotated path flips.
    windata->present_gs = KMSDRM_gbm_surface_create(viddata->gbm_dev, pw, ph, surface_fmt,
                                                    GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!windata->present_gs && errno == ENOSYS) {
        windata->present_gs = KMSDRM_gbm_surface_create(viddata->gbm_dev, pw, ph, surface_fmt, 0);
    }
    if (!windata->present_gs) {
        SDL_SetError("Could not create GBM present surface: %s", strerror(errno));
        goto fail;
    }

    windata->present_egl_surface = egl->eglCreateWindowSurface(egl->egl_display, config,
                                                               (NativeWindowType)windata->present_gs, NULL);
    if (windata->present_egl_surface == EGL_NO_SURFACE) {
        SDL_EGL_SetError("Could not create the EGL present surface", "eglCreateWindowSurface");
        goto fail;
    }

    if (!egl->eglBindAPI(EGL_OPENGL_ES_API)) {
        SDL_EGL_SetError("Could not bind the OpenGL ES API", "eglBindAPI");
        goto fail;
    }
    rot->context = egl->eglCreateContext(egl->egl_display, config, EGL_NO_CONTEXT, context_attribs);
    if (rot->context == EGL_NO_CONTEXT) {
        SDL_EGL_SetError("Could not create the rotate context", "eglCreateContext");
        goto fail;
    }
    if (!egl->eglMakeCurrent(egl->egl_display, windata->present_egl_surface,
                             windata->present_egl_surface, rot->context)) {
        SDL_EGL_SetError("Could not make the rotate context current", "eglMakeCurrent");
        goto fail;
    }
    if (!KMSDRM_RotateGL_Init(&rot->gl)) {
        SDL_SetError("KMSDRM rotated present: could not build the rotate program");
        goto fail;
    }
    egl->eglMakeCurrent(egl->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "KMSDRM rotated present: %d degrees, %dx%d panel, fences %s",
                 rotation, pw, ph, rot->have_fences ? "on" : "off");
    return true;

fail:
    KMSDRM_Rotate_Destroy(_this, window);
    return false;
}

static GLuint KMSDRM_Rotate_TextureForBO(SDL_VideoDevice *_this, struct KMSDRM_Rotate *rot, struct gbm_bo *bo)
{
    KMSDRM_RotateImage *slot;
    int i;

    for (i = 0; i < rot->num_images; ++i) {
        if (rot->images[i].bo == bo) {
            return rot->images[i].texture;
        }
    }
    if (rot->num_images == KMSDRM_ROTATE_MAX_IMAGES) {
        SDL_SetError("KMSDRM rotated present: more application buffers than expected");
        return 0;
    }

    slot = &rot->images[rot->num_images];
    /* EGL_NATIVE_PIXMAP_KHR with a gbm_bo: Mesa's GBM platform duplicates the
       buffer's own DRI image, so this samples the same resource the
       application rendered, with no dma-buf round trip. */
    slot->image = rot->eglCreateImageKHR(_this->egl_data->egl_display, EGL_NO_CONTEXT,
                                         EGL_NATIVE_PIXMAP_KHR, (EGLClientBuffer)bo, NULL);
    if (slot->image == EGL_NO_IMAGE_KHR) {
        SDL_EGL_SetError("Could not create an EGLImage for the application buffer", "eglCreateImageKHR");
        return 0;
    }

    slot->texture = 0;
    rot->gl.GetError(); // Start from a clean error state.
    rot->gl.GenTextures(1, &slot->texture);
    rot->gl.BindTexture(GL_TEXTURE_2D, slot->texture);
    rot->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)slot->image);
    if (!slot->texture || rot->gl.GetError() != GL_NO_ERROR) {
        if (slot->texture) {
            rot->gl.DeleteTextures(1, &slot->texture);
        }
        rot->eglDestroyImageKHR(_this->egl_data->egl_display, slot->image);
        SDL_zerop(slot);
        SDL_SetError("KMSDRM rotated present: could not bind the application buffer as a texture");
        return 0;
    }

    slot->bo = bo;
    ++rot->num_images;
    return slot->texture;
}

bool KMSDRM_Rotate_BeginPresent(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_WindowData *windata = window->internal;
    struct KMSDRM_Rotate *rot = windata->rotate;
    SDL_EGL_VideoData *egl = _this->egl_data;
    KMSDRM_Timing *timing = SDL_GetDisplayDriverDataForWindow(window)->timing;
    Uint64 stage_start = KMSDRM_Timing_Now(timing);
    EGLSyncKHR app_fence = EGL_NO_SYNC_KHR;
    struct gbm_bo *bo;
    GLuint texture;

    if (!rot) {
        return SDL_SetError("KMSDRM rotated present: no present surface");
    }

    // The application's frame, in the application's context.
    if (!egl->eglSwapBuffers(egl->egl_display, windata->egl_surface)) {
        return SDL_EGL_SetError("Failed to swap EGL buffers", "eglSwapBuffers");
    }

    /* Order the rotate pass after everything the application submitted,
       including the swap, without stalling the CPU: a fence here, waited on by
       the rotate context's GPU queue below. */
    if (rot->have_fences) {
        app_fence = egl->eglCreateSyncKHR(egl->egl_display, EGL_SYNC_FENCE_KHR, NULL);
    }
    if (app_fence != EGL_NO_SYNC_KHR) {
        rot->gl.Flush();
    } else {
        rot->gl.Finish();
    }
    KMSDRM_Timing_Add(timing, KMSDRM_TIMING_APP_SWAP, stage_start);

    // SDL_KMSDRM_PRESENT_TIMING=2: how long the application's frame takes to finish on the GPU.
    if (KMSDRM_Timing_Level(timing) >= 2 && app_fence != EGL_NO_SYNC_KHR) {
        stage_start = KMSDRM_Timing_Now(timing);
        egl->eglClientWaitSyncKHR(egl->egl_display, app_fence, 0, EGL_FOREVER_KHR);
        KMSDRM_Timing_Add(timing, KMSDRM_TIMING_APP_GPU, stage_start);
    }
    stage_start = KMSDRM_Timing_Now(timing);

    bo = KMSDRM_gbm_surface_lock_front_buffer(windata->gs);
    if (!bo) {
        if (app_fence != EGL_NO_SYNC_KHR) {
            egl->eglDestroySyncKHR(egl->egl_display, app_fence);
        }
        return SDL_SetError("Failed to lock the application's front buffer");
    }

    egl->eglBindAPI(EGL_OPENGL_ES_API);
    if (!egl->eglMakeCurrent(egl->egl_display, windata->present_egl_surface,
                             windata->present_egl_surface, rot->context)) {
        if (app_fence != EGL_NO_SYNC_KHR) {
            egl->eglDestroySyncKHR(egl->egl_display, app_fence);
        }
        KMSDRM_gbm_surface_release_buffer(windata->gs, bo);
        return SDL_EGL_SetError("Could not make the rotate context current", "eglMakeCurrent");
    }

    if (app_fence != EGL_NO_SYNC_KHR) {
        egl->eglWaitSyncKHR(egl->egl_display, app_fence, 0);
        egl->eglDestroySyncKHR(egl->egl_display, app_fence);
    }

    texture = KMSDRM_Rotate_TextureForBO(_this, rot, bo);
    if (!texture) {
        KMSDRM_gbm_surface_release_buffer(windata->gs, bo);
        return false;
    }

    KMSDRM_RotateGL_Draw(&rot->gl, texture, rot->rotation, rot->pw, rot->ph);

    /* The fence that says this pass no longer reads `bo`. The present's
       eglSwapBuffers flushes too, but flush here so the fence cannot depend
       on a present that fails. */
    rot->frame_bo = bo;
    rot->frame_fence = EGL_NO_SYNC_KHR;
    if (rot->have_fences) {
        rot->frame_fence = egl->eglCreateSyncKHR(egl->egl_display, EGL_SYNC_FENCE_KHR, NULL);
    }
    if (rot->frame_fence != EGL_NO_SYNC_KHR) {
        rot->gl.Flush();
    } else {
        rot->gl.Finish();
    }
    KMSDRM_Timing_Add(timing, KMSDRM_TIMING_ROTATE, stage_start);

    // SDL_KMSDRM_PRESENT_TIMING=2: how long the rotate pass takes to finish on the GPU.
    if (KMSDRM_Timing_Level(timing) >= 2 && rot->frame_fence != EGL_NO_SYNC_KHR) {
        stage_start = KMSDRM_Timing_Now(timing);
        egl->eglClientWaitSyncKHR(egl->egl_display, rot->frame_fence, 0, EGL_FOREVER_KHR);
        KMSDRM_Timing_Add(timing, KMSDRM_TIMING_ROTATE_GPU, stage_start);
    }
    return true;
}

void KMSDRM_Rotate_EndPresent(SDL_VideoDevice *_this, SDL_Window *window, SDL_GLContext app_context)
{
    SDL_WindowData *windata = window->internal;
    struct KMSDRM_Rotate *rot = windata->rotate;

    if (rot && rot->frame_bo) {
        /* One frame of slack: the previous pass has long been submitted, so
           this wait normally returns at once. Only then may the application
           render into that buffer again. */
        if (rot->pending_bo) {
            KMSDRM_Rotate_WaitFence(_this, rot->pending_fence);
            KMSDRM_gbm_surface_release_buffer(windata->gs, rot->pending_bo);
        }
        rot->pending_bo = rot->frame_bo;
        rot->pending_fence = rot->frame_fence;
        rot->frame_bo = NULL;
        rot->frame_fence = EGL_NO_SYNC_KHR;
    }

    SDL_EGL_MakeCurrent(_this, windata->egl_surface, app_context);
}

#endif // SDL_VIDEO_DRIVER_KMSDRM
