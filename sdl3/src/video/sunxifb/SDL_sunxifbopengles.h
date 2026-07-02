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
 * sunxifb GLES wrapper interface.
 *
 * NOTE ON BEAD SPLIT: this HEADER is added in tsp-8si.8 (part 1) only so that
 * SUNXIFB_Create() can take the addresses of the GL_* callbacks and the
 * init/display translation unit compiles cleanly. The IMPLEMENTATION
 * (SDL_sunxifbopengles.c) is tsp-8si.10 (part 3). Until part 3 lands, these
 * symbols are declared-but-undefined, so the library compiles but does not
 * link — exactly the state tsp-8si.8's acceptance criterion describes
 * ("only fails on missing SUNXIFB_GLES_* ... no unrelated compile errors").
 *
 * Mirrors src/video/vivante/SDL_vivanteopengles.h.
 */

#include "SDL_internal.h"

#ifndef SDL_sunxifbopengles_h_
#define SDL_sunxifbopengles_h_

#if defined(SDL_VIDEO_DRIVER_SUNXIFB) && defined(SDL_VIDEO_OPENGL_EGL)

#include "../SDL_sysvideo.h"
#include "../SDL_egl_c.h"

/* OpenGLES functions — thin pass-throughs to the SDL3 core EGL helpers */
#define SUNXIFB_GLES_GetAttribute    SDL_EGL_GetAttribute
#define SUNXIFB_GLES_GetProcAddress  SDL_EGL_GetProcAddressInternal
#define SUNXIFB_GLES_UnloadLibrary   SDL_EGL_UnloadLibrary
#define SUNXIFB_GLES_SetSwapInterval SDL_EGL_SetSwapInterval
#define SUNXIFB_GLES_GetSwapInterval SDL_EGL_GetSwapInterval
#define SUNXIFB_GLES_DestroyContext  SDL_EGL_DestroyContext

extern bool SUNXIFB_GLES_LoadLibrary(SDL_VideoDevice *_this, const char *path);
extern SDL_GLContext SUNXIFB_GLES_CreateContext(SDL_VideoDevice *_this, SDL_Window *window);
extern bool SUNXIFB_GLES_SwapWindow(SDL_VideoDevice *_this, SDL_Window *window);
extern bool SUNXIFB_GLES_MakeCurrent(SDL_VideoDevice *_this, SDL_Window *window, SDL_GLContext context);

#endif /* SDL_VIDEO_DRIVER_SUNXIFB && SDL_VIDEO_OPENGL_EGL */

#endif /* SDL_sunxifbopengles_h_ */
