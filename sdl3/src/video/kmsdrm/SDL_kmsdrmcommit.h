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

/* PocketForge (libsdl3-sunxifb, tsp-mc9m.41.924.16.13): classify the result of
   one drmModeAtomicCommit() for the SDL_KMSDRM_PRESENT_TIMING counters. Like
   SDL_kmsdrmorientation.h, this header is deliberately dependency-free (no SDL,
   DRM or GL), so tests/kmsdrm-rotation includes the exact code the backend runs.

   libdrm's drmModeAtomicCommit() returns the negative errno of a failed
   DRM_IOCTL_MODE_ATOMIC: xf86drmMode.c wraps drmIoctl() in
   `ret < 0 ? -errno : ret` (every libdrm release since atomic support, and
   2.4.114 as shipped on the image). A kernel stall check therefore arrives as
   ret == -EBUSY, with errno == EBUSY as the ioctl left it. The caller also
   passes errno as saved immediately after the call (cleared to 0 before it),
   so a -1 return with errno == EBUSY, the convention of a raw ioctl() or an
   interposed libdrm, is counted as busy too. The return value decides first,
   so a -1 from libdrm (-EPERM) is never mistaken for a busy commit. */

#ifndef SDL_kmsdrmcommit_h_
#define SDL_kmsdrmcommit_h_

#include <errno.h>

typedef enum KMSDRM_CommitResult
{
    KMSDRM_COMMIT_OK,
    KMSDRM_COMMIT_BUSY,   // the previous flip was still pending (-EBUSY)
    KMSDRM_COMMIT_FAILED  // any other failure
} KMSDRM_CommitResult;

static inline KMSDRM_CommitResult KMSDRM_ClassifyCommit(int ret, int saved_errno)
{
    if (ret >= 0) {
        return KMSDRM_COMMIT_OK;
    }
    if (ret == -EBUSY || (ret == -1 && saved_errno == EBUSY)) {
        return KMSDRM_COMMIT_BUSY;
    }
    return KMSDRM_COMMIT_FAILED;
}

#endif // SDL_kmsdrmcommit_h_
