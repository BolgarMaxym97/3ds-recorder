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

/* The end of the APPLICATION memory region (physical), for a frame's address check. On a New 3DS
   in its 124 / 178 MB modes (APPMEMTYPE 6 / 7) the region is that big, while APPMEMALLOC (pm's
   limit for the running title) may say 64 or 80 MB: CTGP-7's Mario Kart 7 drew at pa 26D26E00
   with a limit of 64 MB, every frame was "outside the application's memory" and the video froze
   (issue #25, field logs 2026-11-20). An Old 3DS: the region is the limit. */
static inline uint32_t app_region_end(uint32_t memType, uint32_t appMemAlloc)
{
    uint32_t size = appMemAlloc;
    if (memType == 6 && size < 0x07C00000u)
        size = 0x07C00000u;
    else if (memType == 7 && size < 0x0B200000u)
        size = 0x0B200000u;
    return 0x20000000u + size;
}

/* New 3DS, 124 MB mode: the limit pm left after an application ended. Under the mode's own size it
   stays for the next title that runs in the New 3DS mode: Smash, Ultra Sun / Moon, MH4U and
   Minecraft N3DS started with 64 MB after a Legacy title and crashed or hung (2026-11-20). */
static inline bool app_limit_stuck(uint32_t memType, uint32_t appMemAlloc)
{
    return (memType == 6 && appMemAlloc < 0x07C00000u) || (memType == 7 && appMemAlloc < 0x0B200000u);
}

/* A recording that runs while a program starts takes no frame from the application's memory
   (FCRAM) until the program has loaded (`quiet`): the video keeps its last picture. The program
   maps that memory then, and a frame read from it is a DMA from another process's pages. Consoles
   died that way with a game launched from HOME Menu during a recording (New 3DS Smash, 1.5.0; New
   3DS XL and New 2DS XL in 1.3 / 1.4; field logs 2026-11-19). VRAM frames (HOME Menu) go on.
   `hold`: launch_hold, off by default since 1.6.0 (a test key): the New 3DS launch crashes turned
   out to be the 64 MB limit above (app_limit_stuck), with the recorder idle as well. */
static inline bool front_hold_fcram(bool hold, bool quiet, bool vram)
{
    return hold && quiet && !vram;
}

/* Both screens at one FCRAM address: no game shows one buffer on both, it is still setting its
   screens up (Smash, its first frames: 203A0C80 for both, New 3DS 2026-11-19). Not read then. */
static inline bool screens_unsettled(uint32_t topPa, bool topVram, uint32_t bottomPa, bool bottomVram)
{
    return !topVram && !bottomVram && topPa == bottomPa;
}

/* New 3DS: 804 MHz + L2 only once a program runs and has loaded. Set in HOME Menu, the speed stayed
   for the game started next and through its launch (both consoles that died in a launch ran at it). */
static inline bool cpu_boost_now(uint32_t frontPid, bool quiet)
{
    return frontPid != 0 && !quiet;
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
