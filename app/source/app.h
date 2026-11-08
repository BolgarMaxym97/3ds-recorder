/*
 * The control app's screens (design/ui-studio.html, variant B): control panel, first run,
 * help, settings (language + uninstall for now), uninstall, LED test.
 */
#ifndef APP_H
#define APP_H

#include <3ds.h>
#include <stdbool.h>

#include "config.h"
#include "recorder.h"

typedef struct {
    u32 down, held;
    bool tapped;
    int tap_x, tap_y;
} Input;

typedef enum { SCR_PANEL, SCR_FIRSTRUN, SCR_HELP, SCR_SETTINGS, SCR_HOTKEY, SCR_GALLERY, SCR_PLAYER, SCR_PHONE, SCR_UNINSTALL, SCR_UNINSTALLED, SCR_GUIDE, SCR_LANG } Screen;

typedef struct {
    Screen screen, back;
    int backFocus;          /* focus to restore on the screen help returns to */
    int focus;              /* focused element of the bottom screen */
    int helpTopic;
    int guidePage;
    Screen afterGuide;      /* where the guide goes when it is done */
    bool new3ds;
    Config cfg;
    RecStatus st;
    RecFiles files;
    u32 sdFreeMb, lumaVersion;
    bool lumaModules;
    ModuleFile moduleFile;
    u32 frame;
    char msg[512];   /* UTF-8: Ukrainian takes two bytes a letter */
    u32 msgUntil;           /* frame number */
    bool msgImportant;      /* why a recording stopped: not covered by the next message */
    char msgNext[512];      /* shown when the important one is over */
    /* an action that blocks (start / stop the module) runs one frame after its "busy" text is shown */
    int pending;
    const char *busy;
    bool deleteRecordings, uninstallOk;
    /* hotkey capture: held = buttons down now, cand = the captured combination */
    u32 hkHeld, hkCand, hkSince;
    bool hkReview, hkWaitRelease;
    /* gallery: selected row, first visible row, the list version seen, delete confirmation */
    int galSel, galTop;
    u32 galVersion, galPreviewVersion;
    bool galConfirm;
    bool galDeleting;      /* a deletion runs in the background (no second one meanwhile) */
    int galConfirmFocus;
    char galOpen[32];       /* select this recording when the list arrives (from the panel) */
    bool galAutoPlay;       /* ... and play it */
    u32 playerShowUntil;    /* frame until which the player's controls stay up */
    char phoneName[32];     /* the recording offered to the phone */
    bool quit;
    char selfPath[256];
} App;

void app_init(App *a, const char *selfPath);
void app_update(App *a, const Input *in);
void app_draw_top(App *a);
void app_draw_bottom(App *a);
void app_exit(App *a);

#endif
