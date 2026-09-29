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

#include "SDL_kmsdrmtiming.h"
#include "SDL_kmsdrmcommit.h"

// 0.1 ms buckets up to 250 ms; the last bucket collects everything longer.
#define KMSDRM_TIMING_BUCKET_NS 100000
#define KMSDRM_TIMING_BUCKETS   2501

typedef struct KMSDRM_TimingStat
{
    Uint32 n;
    Uint64 sum_ns;
    Uint64 max_ns;
    Uint32 hist[KMSDRM_TIMING_BUCKETS];
} KMSDRM_TimingStat;

struct KMSDRM_Timing
{
    int level;
    Uint64 last_frame_end_ns;
    Uint32 commit_busy;
    Uint32 commit_fail;
    KMSDRM_TimingStat stats[KMSDRM_TIMING_NUM_STAGES];
};

static const char *const stage_names[KMSDRM_TIMING_NUM_STAGES] = {
    "frame", "app", "app_swap", "app_gpu", "rotate", "rotate_gpu", "present", "flip_wait", "end"
};

KMSDRM_Timing *KMSDRM_Timing_Create(void)
{
    const char *value = SDL_GetHint(SDL_HINT_KMSDRM_PRESENT_TIMING);
    const int level = value ? SDL_atoi(value) : 0;
    KMSDRM_Timing *timing;

    if (level != 1 && level != 2) {
        return NULL;
    }
    timing = (KMSDRM_Timing *)SDL_calloc(1, sizeof(*timing));
    if (timing) {
        timing->level = level;
    }
    return timing;
}

void KMSDRM_Timing_Destroy(KMSDRM_Timing *timing)
{
    SDL_free(timing);
}

int KMSDRM_Timing_Level(const KMSDRM_Timing *timing)
{
    return timing ? timing->level : 0;
}

Uint64 KMSDRM_Timing_Now(const KMSDRM_Timing *timing)
{
    return timing ? SDL_GetTicksNS() : 0;
}

static void KMSDRM_Timing_Record(KMSDRM_Timing *timing, KMSDRM_TimingStage stage, Uint64 ns)
{
    KMSDRM_TimingStat *stat = &timing->stats[stage];
    Uint64 bucket = ns / KMSDRM_TIMING_BUCKET_NS;

    if (bucket >= KMSDRM_TIMING_BUCKETS) {
        bucket = KMSDRM_TIMING_BUCKETS - 1;
    }
    stat->n++;
    stat->sum_ns += ns;
    if (ns > stat->max_ns) {
        stat->max_ns = ns;
    }
    stat->hist[bucket]++;
}

void KMSDRM_Timing_Add(KMSDRM_Timing *timing, KMSDRM_TimingStage stage, Uint64 start_ns)
{
    Uint64 now;

    if (!timing || start_ns == 0 || (unsigned)stage >= (unsigned)KMSDRM_TIMING_NUM_STAGES) {
        return;
    }
    now = SDL_GetTicksNS();
    KMSDRM_Timing_Record(timing, stage, now > start_ns ? now - start_ns : 0);
}

Uint64 KMSDRM_Timing_SwapStart(KMSDRM_Timing *timing)
{
    Uint64 now;

    if (!timing) {
        return 0;
    }
    now = SDL_GetTicksNS();
    if (timing->last_frame_end_ns != 0 && now > timing->last_frame_end_ns) {
        KMSDRM_Timing_Record(timing, KMSDRM_TIMING_APP, now - timing->last_frame_end_ns);
    }
    return now;
}

void KMSDRM_Timing_FrameDone(KMSDRM_Timing *timing)
{
    Uint64 now;

    if (!timing) {
        return;
    }
    now = SDL_GetTicksNS();
    if (timing->last_frame_end_ns != 0 && now > timing->last_frame_end_ns) {
        KMSDRM_Timing_Record(timing, KMSDRM_TIMING_FRAME, now - timing->last_frame_end_ns);
    }
    timing->last_frame_end_ns = now;
}

void KMSDRM_Timing_CountCommit(KMSDRM_Timing *timing, int ret, int saved_errno)
{
    if (!timing) {
        return;
    }
    switch (KMSDRM_ClassifyCommit(ret, saved_errno)) {
    case KMSDRM_COMMIT_BUSY:
        timing->commit_busy++;
        break;
    case KMSDRM_COMMIT_FAILED:
        timing->commit_fail++;
        break;
    default:
        break;
    }
}

// The smallest bucket upper edge that covers `fraction` of the samples.
static double KMSDRM_Timing_Quantile(const KMSDRM_TimingStat *stat, double fraction)
{
    const Uint64 want = (Uint64)SDL_ceil(fraction * (double)stat->n);
    Uint64 seen = 0;
    int i;

    for (i = 0; i < KMSDRM_TIMING_BUCKETS; ++i) {
        seen += stat->hist[i];
        if (seen >= want && seen > 0) {
            if (i == KMSDRM_TIMING_BUCKETS - 1) {
                return (double)stat->max_ns / 1e6;
            }
            return (double)(i + 1) * (double)KMSDRM_TIMING_BUCKET_NS / 1e6;
        }
    }
    return (double)stat->max_ns / 1e6;
}

void KMSDRM_Timing_Report(KMSDRM_Timing *timing, int rotation)
{
    int stage;

    if (!timing || timing->stats[KMSDRM_TIMING_FRAME].n == 0) {
        return;
    }
    SDL_Log("KMSDRM present timing: level=%d rotation=%d frames=%u commit_busy=%u commit_fail=%u",
            timing->level, rotation, (unsigned)timing->stats[KMSDRM_TIMING_FRAME].n,
            (unsigned)timing->commit_busy, (unsigned)timing->commit_fail);
    for (stage = 0; stage < KMSDRM_TIMING_NUM_STAGES; ++stage) {
        const KMSDRM_TimingStat *stat = &timing->stats[stage];
        if (stat->n == 0) {
            continue;
        }
        SDL_Log("KMSDRM present timing: stage=%s n=%u mean_ms=%.3f p50_ms=%.1f p90_ms=%.1f max_ms=%.3f",
                stage_names[stage], (unsigned)stat->n, (double)stat->sum_ns / (double)stat->n / 1e6,
                KMSDRM_Timing_Quantile(stat, 0.5), KMSDRM_Timing_Quantile(stat, 0.9),
                (double)stat->max_ns / 1e6);
    }
    {
        const int level = timing->level;
        SDL_zerop(timing);
        timing->level = level;
    }
}

#endif // SDL_VIDEO_DRIVER_KMSDRM
