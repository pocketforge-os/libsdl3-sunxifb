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
 * sunxifb GLES wrapper implementation (tsp-8si.10, part 3 of 3).
 *
 * Forward-ported from the vendor TrimUI SDL2-2.26.1.GE8300 mali-fbdev backend
 * (src/video/mali-fbdev/SDL_maliopengles.c, 20 lines). Structurally identical
 * to SDL3's in-tree vivante backend (src/video/vivante/SDL_vivanteopengles.c),
 * which is the closest reference: fbdev + EGL + ARM.
 *
 * The pass-through GLES entry points (GetAttribute, GetProcAddress,
 * UnloadLibrary, Set/GetSwapInterval, DestroyContext) are #defined straight to
 * the SDL3 core SDL_EGL_* helpers in SDL_sunxifbopengles.h. Only the four
 * backend-specific entry points are implemented here.
 *
 * SDL2 -> SDL3 deltas applied (per the vendor SDL_maliopengles.c):
 *   - MALI_GLES_LoadLibrary returned int; SUNXIFB_GLES_LoadLibrary returns bool.
 *   - SDL_EGL_LoadLibrary lost its EGLenum `platform` parameter in SDL3: the
 *     vendor 4-arg call SDL_EGL_LoadLibrary(_this, path, EGL_DEFAULT_DISPLAY, 0)
 *     becomes the 3-arg SDL_EGL_LoadLibrary(_this, path, EGL_DEFAULT_DISPLAY).
 *   - The three SDL_EGL_*_impl macros now generate SDL3-shaped functions
 *     (bool / SDL_GLContext returns, window->internal->egl_surface). We expand
 *     them via the macros exactly like vivante does.
 *
 * native DISPLAY handle: we pass EGL_DEFAULT_DISPLAY, matching the vendor SDL2
 * backend. Under the NULL_WSEGL DDK the native display is "use the system
 * default" anyway; EGL_DEFAULT_DISPLAY is the canonical spelling of that.
 */

#include "SDL_internal.h"

#if defined(SDL_VIDEO_DRIVER_SUNXIFB) && defined(SDL_VIDEO_OPENGL_EGL)

#include "SDL_sunxifbopengles.h"
#include "SDL_sunxifbvideo.h"

// EGL implementation of SDL OpenGL support

bool SUNXIFB_GLES_LoadLibrary(SDL_VideoDevice *_this, const char *path)
{
    return SDL_EGL_LoadLibrary(_this, path, EGL_DEFAULT_DISPLAY);
}

SDL_EGL_CreateContext_impl(SUNXIFB)
SDL_EGL_SwapWindow_impl(SUNXIFB)
SDL_EGL_MakeCurrent_impl(SUNXIFB)

#endif // SDL_VIDEO_DRIVER_SUNXIFB && SDL_VIDEO_OPENGL_EGL
