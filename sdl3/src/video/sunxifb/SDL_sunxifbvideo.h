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
 * PocketForge sunxifb backend.
 *
 * Forward-port of the vendor TrimUI SDL2-2.26.1.GE8300 "mali-fbdev" backend
 * (src/video/mali-fbdev/) to SDL3. Despite the historical "mali" name, this is
 * the generic Imagination-fbdev-DDK present path used on the Allwinner A133P
 * (PowerVR Rogue GE8300). Renamed to "sunxifb" for clarity (plan §4).
 *
 * Display path on this SoC: EGL surface -> DC buffer -> dc_sunxi.ko ->
 * Allwinner Display Engine 2.0 -> MIPI-DSI panel. The vendor DDK ships the
 * NULL_WSEGL plugin variant, so PowerVR userspace does NOT open /dev/fb0; we
 * open it only to read the screen geometry, then close it (see VideoInit).
 *
 * Structurally this backend mirrors SDL3's in-tree "vivante" backend
 * (src/video/vivante/), which is the closest reference: fbdev + EGL + ARM.
 */

#include "SDL_internal.h"

#ifndef SDL_sunxifbvideo_h_
#define SDL_sunxifbvideo_h_

#include "../SDL_sysvideo.h"

#include <SDL3/SDL_egl.h>

#include <EGL/egl.h>
#include <linux/vt.h>
#include <linux/fb.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

struct SDL_DisplayData
{
    /*
     * Native window handle handed to eglCreateWindowSurface.
     *
     * LOAD-BEARING: this is intentionally left zero-initialized and is never
     * assigned anywhere. The vendor GE8300 NULL_WSEGL plugin treats a zero
     * EGLNativeWindowType as "use the system default display surface" and
     * routes presentation through the DC buffer / dc_sunxi / DE2.0 path. The
     * vendor SDL2 backend relied on exactly this behaviour (its #ifndef
     * SDL2_POWERVR_GE8300 branch, which used a real `struct fbdev_window`, was
     * dead code referencing a header that does not exist in the vendor tree).
     * Do NOT add an assignment here.
     */
    EGLNativeWindowType native_display;
};

struct SDL_WindowData
{
    /*
     * Must be named exactly `egl_surface`: SDL3's SDL_EGL_*_impl macros
     * (SDL_egl_c.h) expand to code that dereferences window->internal->egl_surface.
     */
    EGLSurface egl_surface;
};

/****************************************************************************/
/* SDL_VideoDevice functions declaration                                    */
/****************************************************************************/

/* Display and window functions */
bool SUNXIFB_VideoInit(SDL_VideoDevice *_this);
void SUNXIFB_VideoQuit(SDL_VideoDevice *_this);
bool SUNXIFB_GetDisplayModes(SDL_VideoDevice *_this, SDL_VideoDisplay *display);
bool SUNXIFB_SetDisplayMode(SDL_VideoDevice *_this, SDL_VideoDisplay *display, SDL_DisplayMode *mode);
bool SUNXIFB_CreateWindow(SDL_VideoDevice *_this, SDL_Window *window, SDL_PropertiesID create_props);
void SUNXIFB_SetWindowTitle(SDL_VideoDevice *_this, SDL_Window *window);
bool SUNXIFB_SetWindowPosition(SDL_VideoDevice *_this, SDL_Window *window);
void SUNXIFB_SetWindowSize(SDL_VideoDevice *_this, SDL_Window *window);
void SUNXIFB_ShowWindow(SDL_VideoDevice *_this, SDL_Window *window);
void SUNXIFB_HideWindow(SDL_VideoDevice *_this, SDL_Window *window);
void SUNXIFB_DestroyWindow(SDL_VideoDevice *_this, SDL_Window *window);

/* Event functions */
void SUNXIFB_PumpEvents(SDL_VideoDevice *_this);

#endif /* SDL_sunxifbvideo_h_ */
