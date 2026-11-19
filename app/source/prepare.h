/*
 * "Prepare for the phone" in the background: a low-priority thread turns recordings into MP4s
 * (prep/, H.264 + AAC) next to them, newest first, while the app is open (convert=manual: only
 * the one being downloaded). A job goes on from
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

/* slices: the H.264 encoder's second half of each picture on core 2 (New 3DS; config mp4_threads).
   autoAll (config convert=auto): every recording, newest first; else only those asked for with
   prepare_first (the Download screen). Replay clips and joins are made either way. */
void prepare_start(bool new3ds, bool slices, bool autoAll);
void prepare_set_auto(bool autoAll);
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
/* Recordings waiting to be joined to the one they continue (join.h): how many, and the progress
   of the one being joined (permille, -1: waiting). */
int prepare_joins(int *permille);
/* The continuation `next` ("yyyymmdd_hhmmss.r3d") could not be joined this session: the gallery
   shows it as a recording of its own. */
bool prepare_join_failed(const char *next);
/* A recording is being encoded right now (the UI then draws less often: it shares the core). */
bool prepare_busy(void);
/* How long the thread has had nothing to do (no clip, join or MP4 left, or held), ms; 0 while it
   works; UINT32_MAX when it does not run. */
u32 prepare_idle_ms(void);
/* "sdmc:/3ds/recorder/recordings/<stem>.<ext>" */
void prepare_path(char *out, size_t n, const char *name, const char *ext);

#endif
