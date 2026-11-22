#include "sdtest.h"

#include <stdlib.h>
#include <string.h>

#include "recorder.h"


#define BLOCK (512u * 1024u)   /* the recorder's write block (sysmod BLOCK) */
#define TEST_FS REC_DIR_FS "/.sdtest.tmp"

static Thread g_thread;
static volatile SdTestResult g_res;

static u32 ms_of(u64 ticks) { return (u32)(ticks * 1000u / SYSCLOCK_ARM11); }

static void test_main(void *arg)
{
    (void)arg;
    const FS_Path path = fsMakePath(PATH_ASCII, TEST_FS);
    Handle f = 0;
    u8 *buf = malloc(BLOCK);
    Result rc = buf ? 0 : -1;
    FSUSER_DeleteFile(recorder_archive(), path);
    if (R_SUCCEEDED(rc))
        rc = FSUSER_OpenFile(&f, recorder_archive(), path, FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
    if (R_SUCCEEDED(rc))   /* made at its size first, as the recorder's next file is */
        rc = FSFILE_SetSize(f, (u64)SDTEST_MB << 20);
    u64 total = 0, slowest = 0;
    const u32 blocks = (SDTEST_MB << 20) / BLOCK;
    for (u32 i = 0; i < blocks && R_SUCCEEDED(rc); i++) {
        for (u32 k = 0; k < BLOCK / 4; k++)   /* not all zeros: some cards treat those faster */
            ((u32 *)buf)[k] = k * 2654435761u + i;
        u32 done = 0;
        const u64 t0 = svcGetSystemTick();
        rc = FSFILE_Write(f, &done, (u64)i * BLOCK, buf, BLOCK, FS_WRITE_FLUSH);
        const u64 dt = svcGetSystemTick() - t0;
        if (R_SUCCEEDED(rc) && done != BLOCK)
            rc = -2;
        total += dt;
        if (dt > slowest)
            slowest = dt;
        g_res.permille = (int)((i + 1) * 1000u / blocks);
    }
    if (f)
        FSFILE_Close(f);
    FSUSER_DeleteFile(recorder_archive(), path);
    free(buf);
    const u32 ms = ms_of(total);
    if (R_SUCCEEDED(rc) && ms) {
        g_res.kbPerSec = (SDTEST_MB << 10) * 1000u / ms;
        g_res.slowestMs = ms_of(slowest);
        app_log("sd test: %u MB in %lu ms, %lu KB/s, slowest block %lu ms", SDTEST_MB, (unsigned long)ms,
                (unsigned long)g_res.kbPerSec, (unsigned long)g_res.slowestMs);
        g_res.state = SDT_DONE;
    } else {
        g_res.rc = rc;
        app_log("sd test failed: %08lX", (unsigned long)rc);
        g_res.state = SDT_FAILED;
    }
}

bool sdtest_start(void)
{
    if (g_thread) {
        if (g_res.state == SDT_RUNNING)
            return false;
        threadJoin(g_thread, U64_MAX);
        threadFree(g_thread);
        g_thread = NULL;
    }
    memset((void *)&g_res, 0, sizeof(g_res));
    g_res.state = SDT_RUNNING;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_thread = threadCreate(test_main, NULL, 16 * 1024, prio - 1, -2, false);   /* above the UI: the card's time, not ours */
    if (!g_thread)
        g_res.state = SDT_IDLE;
    return g_thread != NULL;
}

SdTestResult sdtest_poll(void)
{
    SdTestResult r;
    memcpy(&r, (const void *)&g_res, sizeof(r));
    if (g_thread && r.state != SDT_RUNNING) {
        threadJoin(g_thread, U64_MAX);
        threadFree(g_thread);
        g_thread = NULL;
    }
    return r;
}
