/*
 * "Prepare for the phone" in the background: a low-priority thread turns recordings into MP4s
 * (prep/, H.264 + AAC) next to them, newest first, while the app is open. A job goes on from
 * its last resume point after the app was closed. It pauses while the player runs, while a
 * file goes to the phone, while the module records and while the app is in the background.
 */
#ifndef PREPARE_H
#define PREPARE_H

#include <3ds.h>
#include <stdbool.h>

typedef enum {
    PS_NONE,        /* not known (not a recording on the card) */
    PS_QUEUED,      /* waiting its turn (maybe partly done: permille) */
    PS_WORKING,     /* being encoded now */
    PS_READY,       /* name.mp4 is there */
    PS_FAILED,      /* could not be prepared (this session) */
} PrepareState;

enum { PREP_HOLD_PLAYER = 1, PREP_HOLD_SEND = 2 };

void prepare_start(bool new3ds);
void prepare_stop(void);
/* PREP_HOLD_* bits: work pauses while any is set. */
void prepare_hold(u32 bit, bool on);
/* This recording goes next (now, if another one is being encoded: that one keeps its place). */
void prepare_first(const char *name);
/* State of a recording ("yyyymmdd_hhmmss.r3d"); permille: progress (may be NULL). */
PrepareState prepare_state(const char *name, int *permille);
/* Before a recording is deleted: drops its job and deletes the files made from it. Blocks until done. */
void prepare_forget(const char *name);
/* Replay clips the recorder saved (.rpl) that are not recordings yet: how many, and the progress
   of the one being made into a recording (permille, -1: waiting). Each becomes an ordinary
   recording (prep/unwrap.h) before anything else is prepared. */
int prepare_clips(int *permille);
/* How many clips have become recordings since the app started (the gallery reads its list again
   when this changes). */
u32 prepare_clips_done(void);
/* A recording is being encoded right now (the UI then draws less often: it shares the core). */
bool prepare_busy(void);
/* "sdmc:/3ds/recorder/recordings/<stem>.<ext>" */
void prepare_path(char *out, size_t n, const char *name, const char *ext);

#endif
