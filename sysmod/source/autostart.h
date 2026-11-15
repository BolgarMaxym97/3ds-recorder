/*
 * Launch on startup: the parts that only look at exheaders (the module does the rest, main.c;
 * tests/host/test_autostart.c runs these on a PC).
 *
 * Luma3DS's pm starts NS with its dependencies when the console starts (pm/source/launch.c,
 * loadWithDependencies, v12.0-v13.5). The list is flat and handled by index: NS's own dependencies
 * first, then each started module's dependencies appended at the end. A dependency whose launch
 * fails before its process exists (no file) is skipped, and the failure is forgotten as soon as a
 * later entry starts; only when nothing starts after it does NS go down with it.
 *
 * The recorder goes first into the dependency list of the host, NS's first dependency. It is safe
 * there when the host has one more dependency that nothing has started by then: not a module the
 * FIRM starts itself, not one of NS's own dependencies (those are started before the host's).
 */
#ifndef AUTOSTART_H
#define AUTOSTART_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define AUTO_EXH_SIZE  0x400u   /* ExHeader_Info: what /luma/titles/<id>/exheader.bin must hold */
#define AUTO_EXH_DEPS  0x40u    /* sci.dependencies: 48 title ids */
#define AUTO_EXH_TITLE 0x200u   /* aci.local_caps.title_id */
#define AUTO_MAX_DEPS  48u
#define AUTO_N3DS_MASK 0xF0000000ull
#define AUTO_N3DS_BIT  0x20000000ull

static inline uint64_t auto_dep(const uint8_t *exh, uint32_t i)
{
    uint64_t d;
    memcpy(&d, exh + AUTO_EXH_DEPS + 8u * i, 8);
    return d;
}

static inline uint64_t auto_title(const uint8_t *exh)
{
    uint64_t t;
    memcpy(&t, exh + AUTO_EXH_TITLE, 8);
    return t;
}

/* The dependencies as pm lists them (listDependencies): up to the first empty entry; an Old 3DS
   leaves out New 3DS titles; the New 3DS bits are dropped. Returns their number. */
static inline uint32_t auto_deps(const uint8_t *exh, bool n3ds, uint64_t *out)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < AUTO_MAX_DEPS; i++)
    {
        const uint64_t d = auto_dep(exh, i);
        if (!d)
            break;
        if (n3ds || (d & AUTO_N3DS_MASK) == 0)
            out[n++] = d & ~AUTO_N3DS_MASK;
    }
    return n;
}

/* `tid` is among the exheader's dependencies (New 3DS bits aside). */
static inline bool auto_has(const uint8_t *exh, uint64_t tid)
{
    for (uint32_t i = 0; i < AUTO_MAX_DEPS; i++)
    {
        const uint64_t d = auto_dep(exh, i);
        if (!d)
            break;
        if ((d & ~AUTO_N3DS_MASK) == (tid & ~AUTO_N3DS_MASK))
            return true;
    }
    return false;
}

/* Puts `tid` first, the others move down. False (nothing changed): no room, or it is there. */
static inline bool auto_insert_first(uint8_t *exh, uint64_t tid)
{
    if (auto_dep(exh, AUTO_MAX_DEPS - 1u) != 0 || auto_has(exh, tid))
        return false;
    memmove(exh + AUTO_EXH_DEPS + 8u, exh + AUTO_EXH_DEPS, 8u * (AUTO_MAX_DEPS - 1u));
    memcpy(exh + AUTO_EXH_DEPS, &tid, 8);
    return true;
}

/* sm, fs, pm, loader, pxi: in the FIRM, running before pm starts anything. */
static inline bool auto_in_firm(uint64_t tid)
{
    const uint32_t low = (uint32_t)(tid & ~AUTO_N3DS_MASK);
    return low == 0x00001002u || low == 0x00001102u || low == 0x00001202u || low == 0x00001302u || low == 0x00001402u;
}

/* With `ours` first among the host's dependencies (hostDeps: the host's own, without ours; nsDeps:
   NS's, the host first): a launch of ours that fails is followed by one that pm really makes. */
static inline bool auto_safe(const uint64_t *nsDeps, uint32_t nNs, const uint64_t *hostDeps, uint32_t nHost, uint64_t ours)
{
    for (uint32_t i = 0; i < nHost; i++)
    {
        const uint64_t x = hostDeps[i];
        bool started = x == ours || auto_in_firm(x);
        for (uint32_t k = 0; k < nNs && !started; k++)
            started = nsDeps[k] == x;
        if (!started)
            return true;
    }
    return false;
}

#endif
