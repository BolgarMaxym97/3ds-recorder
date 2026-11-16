#ifndef JOIN_H
#define JOIN_H
/*
 * A recording that the recorder had to pause by itself (an applet took its memory, the lid was
 * closed, HOME in an extended-memory game) goes on in a new file when it can; the recorder then
 * leaves "<next>.join" next to it, naming the session's first file. The app makes them one
 * recording again: the next file's records go after the first file's, JOIN_GAP_US after its end.
 * Portable C99: the app runs it in the background (prepare.c), tests/host/test_join.c on a PC.
 *
 * Nothing is changed in place: the joined recording is written to a new file (`part`) and checked;
 * only then is it put in the first file's place (r3d_join_commit), by renames with a mark file, so
 * that a stop at any moment (the app closed, the power gone) leaves either the two files as they
 * were or the joined one: r3d_join_recover finishes or undoes it the next time.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JOIN_GAP_US    500000u          /* the pause shows as half a second of the last picture */
#define JOIN_MAX_BYTES (3900ull << 20)  /* FAT32 (the recorder's own limit) */
#define JOIN_MAX_US    3600000000u      /* timeUs is 32 bits: the recorder's own limit (an hour) */

enum {
    JOIN_OK = 0,
    JOIN_STOPPED = 1,    /* the progress callback asked to stop: `part` is removed */
    JOIN_DIFFERENT = -1, /* not two closed recordings of the same kind, or too long together: kept apart */
    JOIN_IO = -2,        /* reading or writing failed (the card full?): `part` is removed, both are kept */
};

/* done / total: bytes handled so far; returns nonzero to stop. May be NULL. */
typedef int (*JoinProgress)(void *ctx, uint64_t done, uint64_t total);

/* Writes the joined recording of `first` and `next` to `part`. Neither of them is changed. */
int r3d_join(const char *first, const char *next, const char *part, JoinProgress progress, void *ctx);

/* After JOIN_OK: `part` becomes `first`, `next` is removed. `old` and `mark` are scratch names next
   to them. 0: done; -1: a step failed (r3d_join_recover sorts it out). */
int r3d_join_commit(const char *first, const char *next, const char *part, const char *old, const char *mark);

enum { JOIN_NOTHING = 0, JOIN_FINISHED = 1, JOIN_UNDONE = 2, JOIN_STUCK = -1 };
/* Before anything else with these names: a commit cut short is finished (JOIN_FINISHED) or undone
   with both recordings kept (JOIN_UNDONE); a `part` with no mark (a join cut short) is removed
   (JOIN_NOTHING). JOIN_STUCK: files in a state no step makes; nothing is touched. */
int r3d_join_recover(const char *first, const char *next, const char *part, const char *old, const char *mark);

#ifdef __cplusplus
}
#endif

#endif
