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
#define REC_SHARED_VERSION 3u   /* 2: recovered, recoveredFile; 3: replay (REC_STATE_REPLAY, appBeat, clips) */
#define REC_TITLE_ID       0x0004013000F1D302ULL
#define REC_PROCESS_NAME   "rec3ds"

/* REC_STATE_REPLAY: the replay buffer runs (the last 30 seconds are kept, nothing is being recorded to a file of its own) */
enum { REC_STATE_STARTING, REC_STATE_IDLE, REC_STATE_RECORDING, REC_STATE_SAVING, REC_STATE_ERROR, REC_STATE_EXITED,
       REC_STATE_REPLAY };
/* REC_CMD_RELOAD: the app changed config.ini; read it now, or after the running recording */
/* REC_CMD_SAVE_CLIP: the replay buffer's last 30 seconds become a clip (as the replay hotkey does) */
enum { REC_CMD_NONE, REC_CMD_EXIT, REC_CMD_START, REC_CMD_STOP, REC_CMD_RELOAD, REC_CMD_SAVE_CLIP };

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
    /* replay buffer. appBeat: the app adds 1 every second or so while it is on the screen; the
       buffer pauses while it changes (the app's own work needs the card and the processor). */
    volatile u32 appBeat;
    u32 replayOn;           /* the setting is on (config.ini replay=on) */
    s32 replayError;        /* why the buffer does not run though it is on: 0, or REC_REPLAY_* */
    u32 clips;              /* clips saved since start */
    char lastClip[32];
} RecShared;

enum { REC_REPLAY_OK, REC_REPLAY_NO_SPACE, REC_REPLAY_NO_MEMORY, REC_REPLAY_WRITE_ERROR };

#endif
