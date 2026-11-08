/*
 * The rec3ds sysmodule seen from the app: install, start, stop, status, uninstall.
 * Files it owns:
 *   sdmc:/luma/sysmodules/0004013000F1D302.cxi   the recorder module (copied from romfs)
 *   sdmc:/3ds/recorder/config.ini, recorder.log  settings and log
 *   sdmc:/3ds/recorder/recordings/                recordings
 *   sdmc:/3ds/recorder/rec3ds-launch.3gx         launcher plugin (sysmod: extended-memory games)
 */
#ifndef RECORDER_H
#define RECORDER_H

#include <3ds.h>
#include <stdbool.h>

#define REC_CXI_FS  "/luma/sysmodules/0004013000F1D302.cxi"
#define REC_DIR_FS  "/3ds/recorder"
#define REC_FILES_FS "/3ds/recorder/recordings"
#define REC_LAUNCH_FS "/3ds/recorder/rec3ds-launch.3gx"   /* launcher plugin (extended-memory games, Old 3DS) */
#define REC_APP_LOG "sdmc:/3ds/recorder/app.log"
#define REC_APP_LOG_OLD "sdmc:/3ds/recorder/app.old.log"
#define REC_LOG_MAX (256 * 1024)   /* a log past this moves to *.old.log when the app starts */

/* Appends a time-stamped line to REC_APP_LOG (diagnostics of the app itself). Any thread. */
void app_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* At start-up: rotates the log and writes the console line (model, region, Luma, versions). */
void app_log_start(void);
/* That console line, for the phone page's diagnostics file. */
const char *app_about(void);

typedef enum { MODULE_MISSING, MODULE_DIFFERENT, MODULE_SAME } ModuleFile;

/* What the module reports through its shared page, plus whether it runs at all. */
typedef struct {
    bool running, readable;
    u32 state;               /* REC_STATE_* */
    u32 recSeconds, recKb, framesTop, framesBot, dropped, files;
    Result lastError;
    char lastFile[48];
} RecStatus;

typedef struct {
    u32 count;
    u64 bytes;
    char newest[48];         /* file name, "" if none */
    u64 newestBytes;
} RecFiles;

void recorder_init(FS_Archive sd);
FS_Archive recorder_archive(void); /* the SD card archive opened in main() */
bool recorder_luma_modules_on(void);   /* Luma option "load external FIRMs and modules" */
u32 recorder_luma_version(void);        /* major << 16 | minor << 8 | build, 0 unknown */
ModuleFile recorder_module_file(void);
void recorder_status(RecStatus *out);
/* Recordings the module rebuilt at start-up from ones cut off by power loss / a crash: their
   number (0: none) and the last one's name; reported once. */
int recorder_take_recovered(char *name, size_t cap);
/* The recording that a power-off / restart of the console stopped (System Settings restart it on
   exit), told once: true and its name; *game: a game that needs more memory than usual closed and
   restarted an Old 3DS. */
bool recorder_take_system_stop(char *name, size_t cap, bool *game);
bool recorder_command(u32 cmd);         /* REC_CMD_* */
/* Installs (or updates) the module and starts it. Returns 0 or a Result; *msg names the step. */
Result recorder_install_and_start(const char **step);
bool recorder_stop(void);               /* asks the module to exit, saving a recording first */
void recorder_files(RecFiles *out);
u32 recorder_sd_free_mb(void);
/* Removes the module, settings, log (recordings if asked) and finally this app (3dsx file or
 * CIA title). Returns true when nothing is left. */
bool recorder_uninstall(bool recordings, const char *selfPath);

#endif
