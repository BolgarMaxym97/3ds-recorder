/*
 * A program that has just started is left alone (the module does the rest, main.c;
 * tests/host/test_front.c runs these on a PC).
 *
 * While a game loads, the replay buffer does not start (its memory, its constant SD writes and its
 * frame reads all land on the game's first seconds) and the processor's speed is not touched: NS
 * sets it for the title right then. On a New 3DS, Ultra Moon and Smash did not get through their
 * start with the recorder running (a user's report, 2026-11-14; the cause was not found).
 *
 * Two sources say that a program started: pm's notification 0x10C, and the application's process
 * id changing while the module watches. A program that was already there when the module first
 * looked has an unknown age and counts as old: nothing is held back for it.
 */
#ifndef FRONT_H
#define FRONT_H

#include <stdbool.h>
#include <stdint.h>

#define LAUNCH_QUIET_US 15000000ull   /* a new program is left alone this long */

typedef struct {
    uint32_t pid;        /* the last application seen, 0: none yet */
    uint64_t since;      /* when it appeared */
    bool watching;       /* the module has looked before: a new pid is a new program */
    bool launched;       /* `pid` appeared while the module watched */
    bool noted;          /* 0x10C came */
    uint64_t notedAt;
} FrontApp;

/* The application's pid as the module sees it now (0: none). True: a new program appeared. */
static inline bool front_note(FrontApp *f, uint32_t pid, uint64_t now)
{
    const bool watching = f->watching;
    f->watching = true;
    if (!pid || pid == f->pid)
        return false;
    f->pid = pid;
    f->since = now;
    f->launched = watching;
    return watching;
}

/* pm's 0x10C: an application has started. */
static inline void front_started(FrontApp *f, uint64_t now)
{
    f->noted = true;
    f->notedAt = now;
}

/* A program started less than `quiet` ago (the same unit as `now`). */
static inline bool front_quiet(const FrontApp *f, uint64_t now, uint64_t quiet)
{
    if (f->noted && now - f->notedAt < quiet)
        return true;
    return f->launched && now - f->since < quiet;
}

/* New 3DS: the recorder set 804 MHz + L2 (state 3) while `boostPid` was the application. The
   speed from before goes back only when that same program is still there and the state is still
   the recorder's: otherwise NS has set the speed for another program since, and the old value
   would be wrong for it (268 MHz in a New 3DS game, in HOME Menu). */
static inline bool cpu_restore_ok(uint32_t boostPid, uint32_t nowPid, int nowState)
{
    return nowPid == boostPid && nowState == 3;
}

#endif
