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

#ifndef SDL_kmsdrmrotate_h_
#define SDL_kmsdrmrotate_h_

/* PocketForge (libsdl3-sunxifb): rotated present for panels whose connector
   "panel orientation" is not Normal (the TSP's 720x1280 DSI panel is mounted
   sideways). The DE2 planes of the A133 have no rotation property, so the
   backend gives the application a logical-size (landscape) GBM/EGL surface and
   copies each finished frame, rotated, into a panel-native scanout surface in
   its own GLES2 context. See SDL_kmsdrmorientation.h for the pixel map. */

#include "SDL_kmsdrmvideo.h"

/* Orientation contract (degrees, the convention of
   SDL_PROP_DISPLAY_KMSDRM_PANEL_ORIENTATION_NUMBER):

   - SDL_PROP_DISPLAY_KMSDRM_PANEL_ORIENTATION_NUMBER (display): the connector's
     "panel orientation", always the true value. It is a fact about the panel,
     it exists before any window, and it is what a Vulkan application needs:
     Vulkan display-plane surfaces stay panel-native.
   - SDL_PROP_DISPLAY_KMSDRM_PRESENT_ROTATION_NUMBER (display): the rotation
     this backend applies when it presents a GL window on this display (0 when
     SDL_KMSDRM_PRESENT_ROTATION is "0").
   - SDL_PROP_WINDOW_KMSDRM_PRESENT_ROTATION_NUMBER (window): the rotation SDL
     applies to this window: the display's present rotation for a GL (or
     SDL_Renderer) window, 0 for a Vulkan window.
   - SDL_PROP_WINDOW_KMSDRM_APP_ROTATION_NUMBER (window): the rotation the
     application still owns for this window, i.e. panel orientation minus the
     window's present rotation (mod 360): 0 for a GL window SDL rotates, the
     panel orientation for a Vulkan window or an opted-out GL window. This is
     the value to honour; applying the display's panel orientation to a GL
     window SDL already rotates would rotate it twice. */

// Hint (and environment variable) to turn the rotated present off. "0" makes
// the backend report the panel's native size and leaves the transformation to
// the application (window app_rotation = panel orientation).
#define SDL_HINT_KMSDRM_PRESENT_ROTATION "SDL_KMSDRM_PRESENT_ROTATION"

/* Opt-in source format for the rotated present. "rgb565" makes the
   application's logical drawable a direct 16-bit source for the rotate pass;
   the panel-native present surface remains ARGB8888. Unknown values are
   ignored. The default is ARGB8888. */
#define SDL_HINT_KMSDRM_ROTATE_SOURCE_FORMAT "SDL_KMSDRM_ROTATE_SOURCE_FORMAT"

#define SDL_PROP_DISPLAY_KMSDRM_PRESENT_ROTATION_NUMBER "SDL.display.KMSDRM.pocketforge.present_rotation"
#define SDL_PROP_WINDOW_KMSDRM_PRESENT_ROTATION_NUMBER  "SDL.window.KMSDRM.pocketforge.present_rotation"
#define SDL_PROP_WINDOW_KMSDRM_APP_ROTATION_NUMBER      "SDL.window.KMSDRM.pocketforge.app_rotation"

// Creates the present surface and rotate context for a window whose display
// needs `rotation`. pw x ph is the panel-native mode size. Restores no context.
extern bool KMSDRM_Rotate_Create(SDL_VideoDevice *_this, SDL_Window *window, int rotation, int pw, int ph);

// Destroys everything KMSDRM_Rotate_Create made. The present surface's
// buffers must no longer be on screen or locked. Leaves no context current.
extern void KMSDRM_Rotate_Destroy(SDL_VideoDevice *_this, SDL_Window *window);

/* With the application's context current: swap the application's surface,
   copy that frame rotated into the present surface, and leave the rotate
   context current so the caller presents windata->present_egl_surface /
   windata->present_gs exactly as the unrotated path presents the application's
   surface. */
extern bool KMSDRM_Rotate_BeginPresent(SDL_VideoDevice *_this, SDL_Window *window);

// After the present: return the previous frame's application buffer once the
// rotate pass that read it has finished, and make `app_context` current again.
extern void KMSDRM_Rotate_EndPresent(SDL_VideoDevice *_this, SDL_Window *window, SDL_GLContext app_context);

#endif // SDL_kmsdrmrotate_h_
