/*
 * sdmc:/3ds/recorder/config.ini, shared with the recorder module (which reads it when a
 * recording starts and ignores keys it does not know, such as lang). The app changes single
 * keys and keeps every other line, comments included.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "i18n.h"

#define CONFIG_PATH "sdmc:/3ds/recorder/config.ini"

enum { SCREENS_BOTH, SCREENS_TOP, SCREENS_BOTTOM };

typedef struct {
    int fps;          /* 30, 20, 15 */
    int screens;      /* SCREENS_* */
    int bottomFps;    /* bottom_fps: the bottom screen's own rate with both screens (15, 10, 5, 1); 0: the same as fps */
    bool lossless;    /* quality=lossless: every frame exact; auto (the default): 3D frames in half the bytes */
    bool audio;
    int led;          /* 0 off, 1 solid, 2 pulse */
    char hotkey[48];  /* as written, e.g. R+SELECT */
    Lang lang;        /* lang=<code> (i18n.h LANGS); English when missing or unknown */
    bool langSet;     /* config.ini has lang=: the language was chosen (first run asks) */
    bool guideSeen;   /* guide=done: the first-run guide was shown */
    bool replay;      /* replay=on: the module keeps the last seconds all the time (the replay buffer) */
    int replaySec;    /* replay_sec: how long a clip is (15, 30, 60, 120, 300) */
    bool mainBottom;  /* main_screen=bottom: with both screens, the bottom one keeps its frames first */
    bool inputs;      /* inputs=on (the default): the recorder keeps the buttons pressed (R3DI) */
    bool mp4Inputs;   /* mp4_inputs=on: the console's MP4s show them (prepare.c) */
    char replayHotkey[48]; /* replay_hotkey: saves them as a clip, e.g. X+SELECT */
    bool autostart;   /* autostart=on: the recorder starts with the console (the module sets it up: sysmod AutoApply) */
    bool convertAuto; /* convert=auto (the default): every recording becomes an MP4 in the background; manual: only the one opened in Download (prepare.c) */
    bool lidWork;     /* lid_work=on (the default): this app's background work goes on with the lid closed (lid.h) */
    bool updates;     /* updates=on (the default): the app asks for a newer version when it starts (update.h) */
    bool stats;       /* stats=on (the default): new log lines go to the developer when the app starts (stats.h) */
    bool mp4Slices;   /* mp4_threads=1 (not in the app) turns it off: a New 3DS encodes MP4s on two cores (prepare.c) */
} Config;

void config_load(Config *c);
/* Sets key=value: replaces the key's line, or appends one. Creates the file with the module's
 * defaults when it is missing. */
bool config_set(const char *key, const char *value);

#endif
