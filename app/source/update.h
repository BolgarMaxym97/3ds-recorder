/*
 * In-app update. At launch, with Settings → "Check for updates" on and Wi-Fi, a background
 * thread asks the Worker (STATS_URL/v1/update) for the signed manifest; a newer version shows a
 * dot on the panel's Settings and a row in Settings (app.c, SCR_UPDATE). "Update" downloads the
 * file through the Worker (from the GitHub release) and checks its sha256 from the manifest:
 * the CIA goes into the system as it arrives (AM; cancelled on a wrong byte, so nothing
 * half-installed stays), the 3dsx goes next to the running one and replaces it once it checks.
 * The recorder module and the plugin come with the new app (recorder.c copies them at its start).
 *
 * The request carries nothing about the console: no id, no version.
 */
#ifndef UPDATE_H
#define UPDATE_H

#include <3ds.h>
#include <stdbool.h>

#include "update_core.h"

typedef enum {
    UPD_NONE,         /* nothing newer (or not asked, or the answer did not check) */
    UPD_AVAILABLE,    /* update_info() is newer than this app */
    UPD_DOWNLOADING,
    UPD_INSTALLING,
    UPD_DONE,         /* installed: restart (CIA) or open it again (3dsx) */
    UPD_FAILED,       /* update_error(); "Update" again tries again */
} UpdPhase;

typedef enum {
    UPD_ERR_NONE,
    UPD_ERR_NET,      /* no connection, or it was lost (three tries) */
    UPD_ERR_SERVER,   /* the Worker or GitHub answered with an error */
    UPD_ERR_SPACE,    /* not enough free space on the SD card */
    UPD_ERR_CHECK,    /* the file is not the one the manifest names (size or sha256) */
    UPD_ERR_INSTALL,  /* the system refused the CIA, or the 3dsx could not be replaced */
} UpdError;

typedef enum {
    UPD_CHK_NOT,      /* not asked (the setting is off) */
    UPD_CHK_RUNNING,
    UPD_CHK_NO_WIFI,
    UPD_CHK_FAILED,   /* no answer, or one that does not check */
    UPD_CHK_LATEST,   /* this is the newest */
    UPD_CHK_NEWER,    /* update_phase() went to UPD_AVAILABLE */
} UpdCheck;

/* Launch: starts the check (config.ini updates=on). self: this 3dsx's path (main.c). */
void update_start(bool enabled, const char *self);
/* The setting (checks at the next launches). */
void update_set_enabled(bool enabled);
/* "Check now" on the update screen; false when a check or an install runs. */
bool update_check_now(void);
UpdCheck update_check(void);
UpdPhase update_phase(void);
/* the newer version (valid from UPD_AVAILABLE on) */
const UpdManifest *update_info(void);
/* the bytes this build downloads (the CIA or the 3dsx) */
u32 update_size(void);
/* "What's new" in `lang` (English when it has none). */
int update_notes(const char *lang, char lines[][UPD_NOTE_LEN], int max);
/* Starts the download and install; false when there is nothing to install or it already runs. */
bool update_begin(void);
/* Stops a download (a CIA in the middle is cancelled in the system). */
void update_cancel(void);
/* downloading or installing: sleep is refused, Back does not leave the screen */
bool update_busy(void);
float update_progress(void);   /* 0..1 of the download */
UpdError update_error(void);
u32 update_result(void);       /* the system's code for UPD_ERR_INSTALL / UPD_ERR_NET, 0 when none */
/* App exit: stops the check or the download and waits for its thread. */
void update_stop(void);
#ifdef UI_TOUR
void update_tour(UpdPhase phase, UpdCheck check, UpdError error);
#endif

#endif
