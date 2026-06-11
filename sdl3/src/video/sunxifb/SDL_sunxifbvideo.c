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

/*
 * PocketForge sunxifb backend — init + display lifecycle.
 *
 * Forward-ported from the vendor TrimUI SDL2-2.26.1.GE8300 mali-fbdev backend
 * (src/video/mali-fbdev/SDL_malivideo.c). See SDL_sunxifbvideo.h for the
 * architecture overview.
 *
 * This file is split across three forward-port beads (tsp-8si.8/.9/.10):
 *   - .8 (this file's init/display sections): SUNXIFB_Create/Destroy, the
 *        bootstrap struct, VideoInit/VideoQuit, GetDisplayModes/SetDisplayMode.
 *   - .9: window + EGL surface lifecycle (SUNXIFB_CreateWindow/DestroyWindow,
 *        the window-op stubs, SUNXIFB_PumpEvents).
 *   - .10: the GLES wrapper file (SDL_sunxifbopengles.c) + CMake wiring.
 */

#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_SUNXIFB

/* SDL internals */
#include "../SDL_sysvideo.h"
#include "../../events/SDL_events_c.h"

#ifdef SDL_INPUT_LINUXEV
#include "../../core/linux/SDL_evdev.h"
#endif

#include "SDL_sunxifbvideo.h"
#include "SDL_sunxifbopengles.h"

static void SUNXIFB_Destroy(SDL_VideoDevice *device)
{
    SDL_free(device->internal);
    SDL_free(device);
}

static SDL_VideoDevice *SUNXIFB_Create(void)
{
    SDL_VideoDevice *device;

    /* Initialize SDL_VideoDevice structure */
    device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        return NULL;
    }

    /*
     * The vendor mali-fbdev backend carried no device-private data
     * (device->driverdata == NULL). We keep that: NULL_WSEGL needs no EGL
     * function-pointer table (unlike vivante, which loads fbCreateWindow et al.
     * here). device->internal stays NULL; SUNXIFB_Destroy SDL_free(NULL)s it.
     */
    device->internal = NULL;

    /* Setup amount of available displays */
    device->num_displays = 0;

    /* Set device free function */
    device->free = SUNXIFB_Destroy;

    /* Setup all functions which we can handle */
    device->VideoInit = SUNXIFB_VideoInit;
    device->VideoQuit = SUNXIFB_VideoQuit;
    device->GetDisplayModes = SUNXIFB_GetDisplayModes;
    device->SetDisplayMode = SUNXIFB_SetDisplayMode;
    device->CreateSDLWindow = SUNXIFB_CreateWindow;
    device->SetWindowTitle = SUNXIFB_SetWindowTitle;
    device->SetWindowPosition = SUNXIFB_SetWindowPosition;
    device->SetWindowSize = SUNXIFB_SetWindowSize;
    device->ShowWindow = SUNXIFB_ShowWindow;
    device->HideWindow = SUNXIFB_HideWindow;
    device->DestroyWindow = SUNXIFB_DestroyWindow;

#ifdef SDL_VIDEO_OPENGL_EGL
    device->GL_LoadLibrary = SUNXIFB_GLES_LoadLibrary;
    device->GL_GetProcAddress = SUNXIFB_GLES_GetProcAddress;
    device->GL_UnloadLibrary = SUNXIFB_GLES_UnloadLibrary;
    device->GL_CreateContext = SUNXIFB_GLES_CreateContext;
    device->GL_MakeCurrent = SUNXIFB_GLES_MakeCurrent;
    device->GL_SetSwapInterval = SUNXIFB_GLES_SetSwapInterval;
    device->GL_GetSwapInterval = SUNXIFB_GLES_GetSwapInterval;
    device->GL_SwapWindow = SUNXIFB_GLES_SwapWindow;
    device->GL_DestroyContext = SUNXIFB_GLES_DestroyContext;
#endif

    device->PumpEvents = SUNXIFB_PumpEvents;

    return device;
}

VideoBootStrap SUNXIFB_bootstrap = {
    "sunxifb",
    "Allwinner sunxi-fbdev PowerVR Video Driver",
    SUNXIFB_Create,
    NULL, /* no ShowMessageBox implementation */
    false /* is_preferred */
};

/*****************************************************************************/
/* SDL Video and Display initialization/handling functions                   */
/*****************************************************************************/

bool SUNXIFB_VideoInit(SDL_VideoDevice *_this)
{
    struct fb_var_screeninfo vinfo;
    int fd;
    SDL_VideoDisplay display;
    SDL_DisplayMode current_mode;
    SDL_DisplayData *data;

    data = (SDL_DisplayData *)SDL_calloc(1, sizeof(SDL_DisplayData));
    if (!data) {
        return false;
    }

    /*
     * Open /dev/fb0 ONLY to read the screen geometry, then close it. We never
     * mmap it and never touch /dev/disp: under the NULL_WSEGL DDK, presentation
     * is owned by the PowerVR EGL stack, not by this backend. This mirrors the
     * vendor mali-fbdev backend exactly.
     */
    fd = open("/dev/fb0", O_RDWR, 0);
    if (fd < 0) {
        SDL_free(data);
        return SDL_SetError("sunxifb: Could not open framebuffer device");
    }

    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        close(fd);
        SDL_free(data);
        return SDL_SetError("sunxifb: Could not get framebuffer information");
    }
    close(fd);
    system("setterm -cursor off");

    SDL_zero(current_mode);
    current_mode.w = vinfo.xres;
    current_mode.h = vinfo.yres;
    /* FIXME: Is there a way to tell the actual refresh rate? */
    current_mode.refresh_rate = 60.0f;
    /*
     * Vendor used RGBX8888. Verified plausible against the panel (32 bpp per
     * hardware-firmware-probes.md §5); the live vendor SDL2 testgles2 run
     * negotiated an RGBA8888 EGLConfig (tsp-8si.7). Revisit at G0.5 if the
     * panel shows wrong colours.
     */
    current_mode.format = SDL_PIXELFORMAT_RGBX8888;

    SDL_zero(display);
    display.desktop_mode = current_mode;
    display.internal = data;

    if (SDL_AddVideoDisplay(&display, false) == 0) {
        SDL_free(data);
        return false;
    }

#ifdef SDL_INPUT_LINUXEV
    if (!SDL_EVDEV_Init()) {
        return false;
    }
#endif

    return true;
}

void SUNXIFB_VideoQuit(SDL_VideoDevice *_this)
{
    /* Clear the framebuffer and turn the cursor back on */
    int fd = open("/dev/tty", O_RDWR);
    if (fd >= 0) {
        ioctl(fd, VT_ACTIVATE, 5);
        ioctl(fd, VT_ACTIVATE, 1);
        close(fd);
    }
    system("setterm -cursor on");

#ifdef SDL_INPUT_LINUXEV
    SDL_EVDEV_Quit();
#endif
}

bool SUNXIFB_GetDisplayModes(SDL_VideoDevice *_this, SDL_VideoDisplay *display)
{
    /* Only one display mode available, the current one */
    SDL_AddFullscreenDisplayMode(display, &display->desktop_mode);
    return true;
}

bool SUNXIFB_SetDisplayMode(SDL_VideoDevice *_this, SDL_VideoDisplay *display, SDL_DisplayMode *mode)
{
    return true;
}

/*****************************************************************************/
/* SDL Window creation + EGL surface lifecycle                               */
/*****************************************************************************/

bool SUNXIFB_CreateWindow(SDL_VideoDevice *_this, SDL_Window *window, SDL_PropertiesID create_props)
{
    SDL_WindowData *windowdata;
    SDL_DisplayData *displaydata;

    /*
     * SDL3 takes an SDL_DisplayID, not the SDL2 integer index. Resolve the
     * primary display (we only ever register one in SUNXIFB_VideoInit) and
     * fetch its private data. Mirrors VIVANTE_CreateWindow.
     */
    displaydata = SDL_GetDisplayDriverData(SDL_GetPrimaryDisplay());
    if (!displaydata) {
        return SDL_SetError("sunxifb: no display driver data");
    }

    /* Allocate window internal data */
    windowdata = (SDL_WindowData *)SDL_calloc(1, sizeof(SDL_WindowData));
    if (!windowdata) {
        return false;
    }

    /* Setup driver data for this window (SDL2 driverdata -> SDL3 internal) */
    window->internal = windowdata;

    /*
     * The TSP MIPI-DSI panel is a fixed 1280x720 landscape surface; the
     * Allwinner disp engine rotates it to the 720x1280 portrait panel below
     * userspace (rotate code 3 = 270deg), so apps render landscape and the
     * kernel rotates (build-integration-reference.md §4.5). The vendor backend
     * hardcoded this for the GE8300; keep it.
     */
    window->w = 1280;
    window->h = 720;

    /*
     * Create the EGL window surface only when OpenGL was requested, matching
     * the in-tree vivante backend. SDL3's core (SDL_video.c, SDL_CreateWindow)
     * loads the EGL library via our registered GL_LoadLibrary hook BEFORE this
     * callback runs, but only when SDL_WINDOW_OPENGL is set in the *requested*
     * flags -- so we must not force the flag on here (the vendor SDL2 backend
     * did, but only because it also self-loaded EGL inside CreateWindow; SDL3
     * moved that into the core). Guarding on the flag means egl_data is
     * guaranteed populated whenever we reach SDL_EGL_CreateSurface.
     *
     * displaydata->native_display is the zero-initialized EGLNativeWindowType
     * the NULL_WSEGL DDK treats as "use system default" (see
     * SDL_sunxifbvideo.h). It is intentionally never assigned.
     */
#ifdef SDL_VIDEO_OPENGL_EGL
    if (window->flags & SDL_WINDOW_OPENGL) {
        windowdata->egl_surface = SDL_EGL_CreateSurface(_this, window, (NativeWindowType)displaydata->native_display);
        if (windowdata->egl_surface == EGL_NO_SURFACE) {
            return SDL_SetError("sunxifb: Can't create EGL window surface");
        }
    } else {
        windowdata->egl_surface = EGL_NO_SURFACE;
    }
#endif

    /* One window, it always has focus */
    SDL_SetMouseFocus(window);
    SDL_SetKeyboardFocus(window);

    /* Window has been successfully created */
    return true;
}

void SUNXIFB_DestroyWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_WindowData *data;

    data = window->internal;
    if (data) {
#ifdef SDL_VIDEO_OPENGL_EGL
        if (data->egl_surface != EGL_NO_SURFACE) {
            SDL_EGL_DestroySurface(_this, data->egl_surface);
        }
#endif
        SDL_free(data);
    }
    window->internal = NULL;
}

void SUNXIFB_SetWindowTitle(SDL_VideoDevice *_this, SDL_Window *window)
{
}

bool SUNXIFB_SetWindowPosition(SDL_VideoDevice *_this, SDL_Window *window)
{
    return SDL_Unsupported();
}

void SUNXIFB_SetWindowSize(SDL_VideoDevice *_this, SDL_Window *window)
{
}

void SUNXIFB_ShowWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
}

void SUNXIFB_HideWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
}

/*****************************************************************************/
/* SDL event functions                                                       */
/*****************************************************************************/

void SUNXIFB_PumpEvents(SDL_VideoDevice *_this)
{
#ifdef SDL_INPUT_LINUXEV
    SDL_EVDEV_Poll();
#endif
}

#endif /* SDL_VIDEO_DRIVER_SUNXIFB */
