/*
 * tsp-mc9m.41.924.16.13: unit test of SDL_kmsdrmcommit.h, the classification
 * of one drmModeAtomicCommit() result for the SDL_KMSDRM_PRESENT_TIMING
 * counters (commit_busy / commit_fail).
 *
 * The rows cover both ways a busy commit can arrive:
 *   - real libdrm: drmModeAtomicCommit() returns DRM_IOCTL(), i.e.
 *     `ret < 0 ? -errno : ret` over drmIoctl() (xf86drmMode.c), so -EBUSY with
 *     errno EBUSY;
 *   - a raw-ioctl convention (an interposed or reimplemented libdrm): -1 with
 *     errno EBUSY.
 * The negative rows pin down that the return value decides first: libdrm's
 * -EPERM (-1) and a stale errno must not count as busy.
 *
 * Positive control, in the same run: the rule of 43ee6cb2 (`ret == -EBUSY`
 * only) must disagree with the expected result on the raw-ioctl row, so this
 * table can tell the two rules apart.
 */
#include <errno.h>
#include <stdio.h>

#include "SDL_kmsdrmcommit.h"

typedef struct CommitCase
{
    const char *name;
    int ret;
    int saved_errno;
    KMSDRM_CommitResult expected;
} CommitCase;

static const char *result_name(KMSDRM_CommitResult r)
{
    switch (r) {
    case KMSDRM_COMMIT_OK:
        return "ok";
    case KMSDRM_COMMIT_BUSY:
        return "busy";
    case KMSDRM_COMMIT_FAILED:
        return "failed";
    }
    return "?";
}

// The classification this change replaces (43ee6cb2, SDL_kmsdrmtiming.c).
static KMSDRM_CommitResult old_rule(int ret)
{
    if (ret == 0) {
        return KMSDRM_COMMIT_OK;
    }
    return ret == -EBUSY ? KMSDRM_COMMIT_BUSY : KMSDRM_COMMIT_FAILED;
}

int main(void)
{
    static const CommitCase cases[] = {
        { "success", 0, 0, KMSDRM_COMMIT_OK },
        { "libdrm -EBUSY (errno EBUSY)", -EBUSY, EBUSY, KMSDRM_COMMIT_BUSY },
        { "raw ioctl -1 (errno EBUSY)", -1, EBUSY, KMSDRM_COMMIT_BUSY },
        { "libdrm -EINVAL", -EINVAL, EINVAL, KMSDRM_COMMIT_FAILED },
        { "libdrm -EPERM (-1, errno EPERM)", -1, EPERM, KMSDRM_COMMIT_FAILED },
        { "libdrm -EINVAL early return, stale errno EBUSY", -EINVAL, EBUSY, KMSDRM_COMMIT_FAILED },
        { "raw ioctl -1 (errno EINVAL)", -1, EINVAL, KMSDRM_COMMIT_FAILED },
        { "libdrm -ENOMEM (errno 0)", -ENOMEM, 0, KMSDRM_COMMIT_FAILED },
    };
    int failures = 0, discriminating = 0;
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const CommitCase *c = &cases[i];
        const KMSDRM_CommitResult got = KMSDRM_ClassifyCommit(c->ret, c->saved_errno);
        const int ok = got == c->expected;
        printf("%s: %-48s ret=%d errno=%d -> %s (expected %s)\n", ok ? "ok" : "FAIL", c->name, c->ret,
               c->saved_errno, result_name(got), result_name(c->expected));
        failures += !ok;
        if (old_rule(c->ret) != c->expected) {
            ++discriminating;
            printf("    control: the 43ee6cb2 rule gives %s here\n", result_name(old_rule(c->ret)));
        }
    }
    if (discriminating == 0) {
        printf("FAIL: no row separates the new classification from the 43ee6cb2 rule\n");
        ++failures;
    } else {
        printf("ok: %d row(s) the 43ee6cb2 rule gets wrong\n", discriminating);
    }
    printf("RESULT: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
