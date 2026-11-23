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

/* Whether a screen's picture is taken at a capture slot: a new swap, a still VRAM screen's
   refresh, a late capture, or a swap still pending (its capture found the slots busy, or the mode
   had to settle) once the screen has kept it for staticGapUs. Without the last one a game that
   stopped drawing kept the picture before its last swap in the video. */
static inline bool capture_trigger(bool isNew, bool isStatic, bool late, bool pend, uint64_t sinceSwapUs,
                                   uint32_t staticGapUs)
{
    return isNew || isStatic || late || (pend && sinceSwapUs >= staticGapUs);
}

/* CTRPluginFramework's menu (a 3GX plugin's own screens, issue #21): it draws into two buffers per
   screen of its own at VA 0x06000000 in the game, RGB565, 240 rows of 480 bytes, top screen in
   the 2D mode; the displayed address is outside the game's linear memory, so it was skipped.
   From the Yu edition (Yu08083/3ds-recorder, sysmod/source/ctrpf_display.h). */
#define CTRPF_VA 0x06000000u
static inline bool ctrpf_display_layout(int sc, uint32_t fmt, uint32_t stride, uint32_t size)
{
    if ((sc != 0 && sc != 1) || (fmt & 7u) != 2u || stride != 480u)
        return false;
    if (sc == 0 && (fmt & 0x60u) != 0x40u)
        return false;
    return size == (sc == 0 ? 400u : 320u) * 480u;
}

static inline uint32_t ctrpf_display_buffer(int sc, unsigned index)
{
    const uint32_t top = 400u * 480u, bottom = 320u * 480u;
    return sc == 0 ? CTRPF_VA + index * top : CTRPF_VA + 2u * top + index * bottom;
}

/* Which of a screen's two CTRPF buffers the screen shows, from the addresses it shows alone: the
   plugin's memory is one block, so buffer 1 is one buffer above buffer 0 in physical memory too
   (06000000 -> 256FD000, 0602EE00 -> 2572BE00, O3DS logs 2026-11-22). No asking Luma for the
   physical address (GET_PA_FROM_VA walks the game's page tables without a lock: the kernel went down
   three times, Mario Kart 7 with its plugin's menu, O3DS 2026-11-22/23). An address not one buffer
   above the lowest seen starts again from it as buffer 0; a wrong first guess shows the menu's other,
   previous picture until the lower buffer comes. lo: per screen, 0 at a new game. */
typedef struct {
    uint32_t lo[2];
} CtrpfPick;
static inline unsigned ctrpf_pick(CtrpfPick *p, int sc, uint32_t pa)
{
    const uint32_t step = sc == 0 ? 400u * 480u : 320u * 480u;
    uint32_t *lo = &p->lo[sc ? 1 : 0];
    if (pa != *lo && pa != *lo + step)
        *lo = pa;
    return pa == *lo ? 0u : 1u;
}

/* A framebuffer the kernel refused to DMA from (D04007F0) is left alone for a moment, that one
   only: all of the application's frames were skipped for 0.5 s after any refusal, 7100 frames on
   New 3DS consoles in a day of field logs (2026-11-21), most of them from buffers that read fine. */
#define PA_BACKOFF_N 4
typedef struct {
    uint32_t pa[PA_BACKOFF_N];
    uint64_t until[PA_BACKOFF_N];
} PaBackoff;

static inline bool pa_backoff_active(const PaBackoff *b, uint32_t pa, uint64_t now)
{
    for (int i = 0; i < PA_BACKOFF_N; i++)
        if (b->pa[i] == pa && now < b->until[i])
            return true;
    return false;
}

static inline void pa_backoff_set(PaBackoff *b, uint32_t pa, uint64_t until)
{
    int at = 0;
    for (int i = 0; i < PA_BACKOFF_N; i++)
    {
        if (b->pa[i] == pa)
        {
            at = i;
            break;
        }
        if (b->until[i] < b->until[at])
            at = i;   /* else the one that ends first */
    }
    b->pa[at] = pa;
    b->until[at] = until;
}

/* With both screens, the second one (not the main_screen) waits while the main one has two frames
   for the converter still waiting, as long as it got one in the last gap: 10 fps (Flashback v4's
   notes go down to a picture a second; 5 fps was tried on an Old 3DS and only lost the second screen
   its frames). */
#define SECOND_GAP_N3DS_US 100000u
#define SECOND_GAP_O3DS_US 100000u   /* 5 fps (200 ms) left the bottom screen of Mario Kart 7 at 9 fps and won the top
                                         one nothing (25.7 vs 25.2 fps), O3DS 2026-11-22: 10 fps as before */
static inline uint32_t second_gap_us(bool isNew3ds) { return isNew3ds ? SECOND_GAP_N3DS_US : SECOND_GAP_O3DS_US; }
static inline bool second_yields(bool isSecond, bool both, uint32_t mainBusy, uint64_t sinceLastUs, uint32_t gapUs)
{
    return isSecond && both && mainBusy >= 2u && sinceLastUs < gapUs;
}

#endif
