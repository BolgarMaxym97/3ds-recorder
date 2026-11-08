#ifndef RECORDER_SHARED_H
#define RECORDER_SHARED_H
/*
 * Status/control page of the recorder sysmodule, shared with the app.
 * The module keeps it in its own 4 KB page; the app maps that page from the
 * module process (svcMapProcessMemoryEx) to read the status and to post a
 * command. Address exported by sysmod/Makefile (nm) into app/source/shared_addr.h.
 */
#include <3ds/types.h>

#define REC_SHARED_MAGIC   0x43455233u   /* "3REC" */
#define REC_SHARED_VERSION 2u   /* 2: recovered, recoveredFile */
#define REC_TITLE_ID       0x0004013000F1D302ULL
#define REC_PROCESS_NAME   "rec3ds"

enum { REC_STATE_STARTING, REC_STATE_IDLE, REC_STATE_RECORDING, REC_STATE_SAVING, REC_STATE_ERROR, REC_STATE_EXITED };
/* REC_CMD_RELOAD: the app changed config.ini; read it now, or after the running recording */
enum { REC_CMD_NONE, REC_CMD_EXIT, REC_CMD_START, REC_CMD_STOP, REC_CMD_RELOAD };

typedef struct {
    u32 magic, version;
    volatile u32 state;     /* REC_STATE_* (module) */
    volatile u32 cmd;       /* REC_CMD_* (app writes, module clears) */
    u32 recSeconds, recKb, framesTop, framesBot, dropped, throttled;
    u32 files;              /* recordings finished since start */
    s32 lastError;
    u32 sysFreeKb, ringKb, sdFreeMb;
    u32 initStage;          /* start-up progress, for diagnosis */
    char lastFile[48];
    /* recordings cut off by power loss or a crash, made into .r3d files at start-up; the app
       shows it once and sets recovered back to 0 */
    volatile u32 recovered;
    char recoveredFile[32];
} RecShared;

#endif
