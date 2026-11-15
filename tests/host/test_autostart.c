/* Launch on startup (sysmod/source/autostart.h): the dependency list as pm reads it, the recorder
   put first into the host's, and the rule that says when a missing recorder cannot stop the
   console's start, checked against a model of pm's loop (Luma3DS pm/source/launch.c,
   loadWithDependencies). The Old 3DS lists are the ones read on the console (2026-11-15). */
#include "autostart.h"

#include <stdio.h>

static int g_fail;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            g_fail = 1;                                                  \
        }                                                                \
    } while (0)

#define SYS(low) (0x0004013000000000ull | (low))
#define OURS SYS(0xF1D302u)

typedef struct {
    uint64_t tid;
    uint32_t n;
    uint64_t deps[8];
} Mod;

static const Mod *find(const Mod *mods, uint32_t count, uint64_t tid)
{
    for (uint32_t i = 0; i < count; i++)
        if (mods[i].tid == tid)
            return &mods[i];
    return NULL;
}

/* pm's loop: true when NS stays up although `missing` cannot be launched. */
static bool pm_starts(const Mod *mods, uint32_t count, uint64_t root, uint64_t missing)
{
    uint64_t list[AUTO_MAX_DEPS], running[64];
    uint32_t num = 0, nRun = 0;
    bool ok = true;
    for (uint32_t low = 0x1002u; low <= 0x1402u; low += 0x100u)
        running[nRun++] = SYS(low);
    const Mod *m = find(mods, count, root);
    for (uint32_t pass = 0;; pass++)
    {
        for (uint32_t i = 0; m && i < m->n; i++)   /* listMergeUniqueDependencies */
        {
            uint32_t j = 0;
            for (; j < num && list[j] != m->deps[i]; j++)
                ;
            if (j == num)
                list[num++] = m->deps[i];
        }
        /* the next entry that is not running yet */
        m = NULL;
        static uint32_t at;
        if (pass == 0)
            at = 0;
        for (; at < num && !m; at++)
        {
            bool up = false;
            for (uint32_t k = 0; k < nRun; k++)
                up = up || running[k] == list[at];
            if (up)
                continue;
            if (list[at] == missing)
            {
                ok = false;   /* res = failure; no process: the loop goes on */
                continue;
            }
            ok = true;        /* res = this launch's */
            running[nRun++] = list[at];
            m = find(mods, count, list[at]);
            if (!m)
            {
                static const Mod leaf = {0, 0, {0}};
                m = &leaf;
            }
        }
        if (!m)
            return ok;
    }
}

static void exh_of(const Mod *m, uint8_t *exh)
{
    memset(exh, 0, AUTO_EXH_SIZE);
    memcpy(exh + AUTO_EXH_TITLE, &m->tid, 8);
    for (uint32_t i = 0; i < m->n; i++)
        memcpy(exh + AUTO_EXH_DEPS + 8u * i, &m->deps[i], 8);
}

int main(void)
{
    uint8_t exh[AUTO_EXH_SIZE];
    uint64_t nsDeps[AUTO_MAX_DEPS], hostDeps[AUTO_MAX_DEPS];

    /* Old 3DS, firmware 11.17: NS, cfg, ptm as read through loader */
    Mod o3ds[] = {
        {SYS(0x8002u), 2, {SYS(0x2202u), SYS(0x1702u)}},
        {SYS(0x1702u), 2, {SYS(0x3102u), SYS(0x2302u)}},
        {SYS(0x2202u), 6, {SYS(0x1702u), SYS(0x1B02u), SYS(0x1E02u), SYS(0x1F02u), SYS(0x2102u), SYS(0x2302u)}},
    };
    exh_of(&o3ds[0], exh);
    const uint32_t nNs = auto_deps(exh, false, nsDeps);
    CHECK(nNs == 2 && nsDeps[0] == SYS(0x2202u));
    exh_of(&o3ds[2], exh);
    const uint32_t nHost = auto_deps(exh, false, hostDeps);
    CHECK(nHost == 6);
    CHECK(auto_safe(nsDeps, nNs, hostDeps, nHost, OURS));
    CHECK(!auto_has(exh, OURS));
    CHECK(auto_insert_first(exh, OURS));
    CHECK(auto_has(exh, OURS) && auto_dep(exh, 0) == OURS && auto_dep(exh, 1) == SYS(0x1702u) && auto_dep(exh, 6) == SYS(0x2302u));
    CHECK(auto_title(exh) == SYS(0x2202u));
    CHECK(!auto_insert_first(exh, OURS));   /* once */
    /* pm with the recorder in ptm's list: with its file, and without */
    Mod patched = o3ds[2];
    patched.n = 7;
    for (uint32_t i = 0; i < 7; i++)
        patched.deps[i] = auto_dep(exh, i);
    Mod with[] = {o3ds[0], o3ds[1], patched};
    CHECK(pm_starts(with, 3, SYS(0x8002u), 0));
    CHECK(pm_starts(with, 3, SYS(0x8002u), OURS));

    /* a host whose own dependencies are all NS's or the FIRM's: nothing starts after ours */
    Mod bad[] = {
        {SYS(0x8002u), 2, {SYS(0x2202u), SYS(0x1702u)}},
        {SYS(0x2202u), 2, {SYS(0x1702u), SYS(0x1102u)}},
    };
    exh_of(&bad[0], exh);
    const uint32_t nNs2 = auto_deps(exh, false, nsDeps);
    exh_of(&bad[1], exh);
    const uint32_t nHost2 = auto_deps(exh, false, hostDeps);
    CHECK(!auto_safe(nsDeps, nNs2, hostDeps, nHost2, OURS));
    Mod badWith[] = {bad[0], {SYS(0x2202u), 3, {OURS, SYS(0x1702u), SYS(0x1102u)}}};
    CHECK(pm_starts(badWith, 2, SYS(0x8002u), 0));
    CHECK(!pm_starts(badWith, 2, SYS(0x8002u), OURS));   /* the case the rule keeps away from */

    /* New 3DS titles in a list: an Old 3DS leaves them out, a New 3DS drops the bits */
    Mod mixed = {SYS(0x20008002u), 3, {SYS(0x20002202u), SYS(0x1702u), SYS(0x20003302u)}};
    exh_of(&mixed, exh);
    CHECK(auto_deps(exh, false, nsDeps) == 1 && nsDeps[0] == SYS(0x1702u));
    CHECK(auto_deps(exh, true, nsDeps) == 3 && nsDeps[0] == SYS(0x2202u) && nsDeps[2] == SYS(0x3302u));
    CHECK(auto_has(exh, SYS(0x2202u)));   /* bits aside */

    /* a full list takes no more */
    memset(exh, 0, sizeof(exh));
    for (uint32_t i = 0; i < AUTO_MAX_DEPS; i++)
    {
        const uint64_t d = SYS(0x1002u + 0x100u * i);
        memcpy(exh + AUTO_EXH_DEPS + 8u * i, &d, 8);
    }
    uint8_t before[AUTO_EXH_SIZE];
    memcpy(before, exh, sizeof(exh));
    CHECK(!auto_insert_first(exh, OURS) && memcmp(before, exh, sizeof(exh)) == 0);

    /* nothing outside the dependency list moves */
    memset(exh, 0xA5, sizeof(exh));
    memset(exh + AUTO_EXH_DEPS, 0, 8u * AUTO_MAX_DEPS);
    const uint64_t one = SYS(0x1B02u);
    memcpy(exh + AUTO_EXH_DEPS, &one, 8);
    memcpy(before, exh, sizeof(exh));
    CHECK(auto_insert_first(exh, OURS));
    CHECK(memcmp(before, exh, AUTO_EXH_DEPS) == 0);
    CHECK(memcmp(before + AUTO_EXH_DEPS + 8u * AUTO_MAX_DEPS, exh + AUTO_EXH_DEPS + 8u * AUTO_MAX_DEPS,
                 AUTO_EXH_SIZE - AUTO_EXH_DEPS - 8u * AUTO_MAX_DEPS) == 0);
    CHECK(auto_dep(exh, 0) == OURS && auto_dep(exh, 1) == one && auto_dep(exh, 2) == 0);

    if (!g_fail)
        printf("test_autostart: ok\n");
    return g_fail;
}
