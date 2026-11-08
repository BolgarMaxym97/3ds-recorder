#ifndef FBADDR_H
#define FBADDR_H

/*
 * Framebuffer address translation for DMA.
 *
 * CTRPF reports game framebuffers as 0x80000000 | PA. svcStartInterProcessDma
 * rejects that alias (0xE0E01BF5, invalid address): it needs a normal virtual
 * address of the source process. Games keep framebuffers either in VRAM
 * (NSMB2) or in FCRAM (SM3DL), so the VA is found by trying the usual
 * mappings and keeping the one the kernel translates back to the same PA.
 */

#include <3ds.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Luma custom SVC (implemented in libctrpf): VA -> PA in the current process, 0 if unmapped. */
u32 svcConvertVAToPA(const void *VA, bool writeCheck);

static inline u32 fb_alias_to_pa(u32 fb)
{
    return fb & 0x7FFFFFFFu;
}

static inline bool fb_pa_in_vram(u32 pa)
{
    return pa >= 0x18000000u && pa < 0x18600000u;
}

static inline bool fb_pa_in_fcram(u32 pa)
{
    return pa >= 0x20000000u && pa < 0x30000000u;
}

/*
 * Returns a VA in the current (game) process that maps to the framebuffer's PA,
 * or 0. `which` (optional) gets 1 = VRAM 0x1F000000, 2 = linear 0x30000000,
 * 3 = linear 0x14000000.
 */
static inline u32 fb_resolve_game_va(u32 fb, int *which)
{
    const u32 pa = fb_alias_to_pa(fb);
    u32 candidates[3] = { 0, 0, 0 };
    if (fb_pa_in_vram(pa))
        candidates[0] = 0x1F000000u + (pa - 0x18000000u);
    if (fb_pa_in_fcram(pa))
    {
        candidates[1] = 0x30000000u + (pa - 0x20000000u);
        candidates[2] = 0x14000000u + (pa - 0x20000000u);
    }
    for (int i = 0; i < 3; i++)
    {
        const u32 va = candidates[i];
        /* Compare page-aligned: the PA of the page plus the offset inside it. */
        if (va && svcConvertVAToPA((const void *)(va & ~0xFFFu), false) == (pa & ~0xFFFu))
        {
            if (which)
                *which = i + 1;
            return va;
        }
    }
    if (which)
        *which = 0;
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* FBADDR_H */
