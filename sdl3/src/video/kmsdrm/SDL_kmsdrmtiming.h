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

#ifndef SDL_kmsdrmtiming_h_
#define SDL_kmsdrmtiming_h_

/* PocketForge (libsdl3-sunxifb, tsp-mc9m.41.924.16.13): opt-in per-stage
   timing of the GL present path, so a device run can say where a frame's time
   goes without a profiler.

   SDL_KMSDRM_PRESENT_TIMING (hint or environment variable):
     unset, 0  nothing is allocated, recorded or logged;
     1         the wall time of every present stage is recorded, and one
               summary line per stage is logged (SDL_Log) when the window is
               destroyed;
     2         as 1, and the CPU also waits for each GPU pass to finish
               (app_gpu, rotate_gpu). That serialises CPU and GPU and so
               lowers the frame rate: it is a decomposition run, never the
               headline number.

   Summary format (one line each, parseable):
     KMSDRM present timing: level=L rotation=R frames=N commit_busy=B commit_fail=F
     KMSDRM present timing: stage=S n=N mean_ms=M p50_ms=P p90_ms=Q max_ms=X */
#define SDL_HINT_KMSDRM_PRESENT_TIMING "SDL_KMSDRM_PRESENT_TIMING"

typedef enum KMSDRM_TimingStage
{
    KMSDRM_TIMING_FRAME,      // end of one present to the end of the next
    KMSDRM_TIMING_APP,        // end of a present to the next swap request (the application's own work)
    KMSDRM_TIMING_APP_SWAP,   // rotated: eglSwapBuffers of the application surface, its fence, its flush
    KMSDRM_TIMING_APP_GPU,    // level 2: CPU wait until the application's frame has finished on the GPU
    KMSDRM_TIMING_ROTATE,     // rotated: switch to the rotate context, record and flush the rotate pass
    KMSDRM_TIMING_ROTATE_GPU, // level 2, rotated: CPU wait until the rotate pass has finished on the GPU
    KMSDRM_TIMING_PRESENT,    // the swap path: native fence, eglSwapBuffers, atomic commit (includes flip_wait)
    KMSDRM_TIMING_FLIP_WAIT,  // waiting for the previous page flip before the next atomic commit
    KMSDRM_TIMING_END,        // rotated: hand back the previous application buffer, switch back to its context
    KMSDRM_TIMING_NUM_STAGES
} KMSDRM_TimingStage;

typedef struct KMSDRM_Timing KMSDRM_Timing;

// NULL (timing off) unless SDL_KMSDRM_PRESENT_TIMING is 1 or 2.
extern KMSDRM_Timing *KMSDRM_Timing_Create(void);
extern void KMSDRM_Timing_Destroy(KMSDRM_Timing *timing);

// Every function below accepts NULL and then does nothing (and returns 0).
extern int KMSDRM_Timing_Level(const KMSDRM_Timing *timing);
extern Uint64 KMSDRM_Timing_Now(const KMSDRM_Timing *timing);
// Records now - start_ns for `stage`. A start of 0 (timing was off) records nothing.
extern void KMSDRM_Timing_Add(KMSDRM_Timing *timing, KMSDRM_TimingStage stage, Uint64 start_ns);
// A swap request: records the application stage since the last present and returns now.
extern Uint64 KMSDRM_Timing_SwapStart(KMSDRM_Timing *timing);
// The end of a present: records the frame stage.
extern void KMSDRM_Timing_FrameDone(KMSDRM_Timing *timing);
// The result of one drmModeAtomicCommit(): its return value and errno as saved
// immediately after the call (see SDL_kmsdrmcommit.h for the classification).
extern void KMSDRM_Timing_CountCommit(KMSDRM_Timing *timing, int ret, int saved_errno);
// Logs the summary if any frame was recorded, then starts over.
extern void KMSDRM_Timing_Report(KMSDRM_Timing *timing, int rotation);

#endif // SDL_kmsdrmtiming_h_
