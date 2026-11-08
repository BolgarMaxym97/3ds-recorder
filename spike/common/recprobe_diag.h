#ifndef RECPROBE_DIAG_H
#define RECPROBE_DIAG_H
/*
 * Progress page of the recprobe sysmodule (spike), shared with the launcher.
 * The module keeps it in its own 4 KB page; the launcher maps that page from
 * the module process (svcMapProcessMemoryEx) and shows it live, so start-up
 * problems are visible without SD or LED.
 */
#include <3ds/types.h>

#define RECPROBE_DIAG_MAGIC 0x47445052u   /* "RPDG" */

typedef struct {
    u32 magic;
    u32 stage;        /* RECPROBE_STAGE_* */
    u32 srvTries;
    s32 rc[10];       /* see RECPROBE_RC_* */
    u32 seconds, captures, writtenKb, ringKb, probeMaxKb, sysFreeKb, phase;
} RecprobeDiag;

enum {
    RECPROBE_RC_HEAP, RECPROBE_RC_SRV, RECPROBE_RC_FS, RECPROBE_RC_SDMC, RECPROBE_RC_MCU,
    RECPROBE_RC_MCU_STEAL, RECPROBE_RC_LED, RECPROBE_RC_SETUP, RECPROBE_RC_FILE, RECPROBE_RC_MKDIR,
};

enum {
    RECPROBE_STAGE_INIT = 1, RECPROBE_STAGE_SYSCALLS, RECPROBE_STAGE_HEAP, RECPROBE_STAGE_SRV,
    RECPROBE_STAGE_FS, RECPROBE_STAGE_SDMC, RECPROBE_STAGE_LIBC,
    RECPROBE_STAGE_MAIN = 10, RECPROBE_STAGE_DIRS, RECPROBE_STAGE_INFO, RECPROBE_STAGE_LED,
    RECPROBE_STAGE_SETUP, RECPROBE_STAGE_FILE, RECPROBE_STAGE_THREADS,
    RECPROBE_STAGE_POLL = 20, RECPROBE_STAGE_DRAIN = 30, RECPROBE_STAGE_DONE = 40, RECPROBE_STAGE_FAIL = 99,
};

static inline const char *recprobe_stage_name(u32 s)
{
    switch (s) {
    case RECPROBE_STAGE_INIT: return "init: start";
    case RECPROBE_STAGE_SYSCALLS: return "init: syscalls";
    case RECPROBE_STAGE_HEAP: return "init: heap";
    case RECPROBE_STAGE_SRV: return "init: srv";
    case RECPROBE_STAGE_FS: return "init: fs";
    case RECPROBE_STAGE_SDMC: return "init: sdmc";
    case RECPROBE_STAGE_LIBC: return "init: done";
    case RECPROBE_STAGE_MAIN: return "main";
    case RECPROBE_STAGE_DIRS: return "folders";
    case RECPROBE_STAGE_INFO: return "info.txt";
    case RECPROBE_STAGE_LED: return "LED";
    case RECPROBE_STAGE_SETUP: return "memory";
    case RECPROBE_STAGE_FILE: return "rec file";
    case RECPROBE_STAGE_THREADS: return "threads";
    case RECPROBE_STAGE_POLL: return "RUNNING";
    case RECPROBE_STAGE_DRAIN: return "saving";
    case RECPROBE_STAGE_DONE: return "DONE";
    case RECPROBE_STAGE_FAIL: return "FAILED";
    default: return "?";
    }
}

#endif
