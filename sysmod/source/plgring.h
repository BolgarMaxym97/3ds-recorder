/*
 * The ring in the launcher plugin's memory (an extended-memory game on an Old 3DS) and its mirror
 * (the module does the rest, main.c; tests/host/test_plgring.c runs these on a PC).
 *
 * The ring's first bytes are mapped a second time right after the plugin's memory, where the ring
 * ends, so a record that runs past the end is already at the start. Without the mirror the rest of
 * the ring is a pad record written to the card: 9-16% of a recording's bytes with this small ring.
 *
 * The kernel maps such a mirror once: after an unmap, mapping one again failed every time
 * (E0A01BF5; the second recording in one run of Smash, O3DS 0.12.1; a recording after the replay
 * buffer, whose ring was 1024 KB against the recording's 768 KB, O3DS 1.2.0, 2026-11-15). So the
 * mirror is mapped once per game, and the ring has one place there whatever is being recorded:
 * its last PLG_RING_BYTES. A ring that cannot be there (less memory left) works with pad records.
 */
#ifndef PLGRING_H
#define PLGRING_H

#include <stdbool.h>
#include <stdint.h>

/* What a recording of its own leaves for the ring there (three top-screen frames, 2.5 s of audio);
   the replay buffer leaves more and gets the same. A whole number of the writer's blocks. */
#define PLG_RING_BYTES (768u * 1024u)

/* The ring's size with `left` bytes free below the end of the plugin's memory: whole blocks, and
   never more than the one size the mirror is for. */
static inline uint32_t plg_ring_size(uint32_t left, uint32_t block)
{
    const uint32_t size = left / block * block;
    return size > PLG_RING_BYTES ? PLG_RING_BYTES : size;
}

/* Where the ring lies in a plugin memory of `plgSize` bytes (*ringOff), for mapping the mirror of
   its first `over` bytes. False: no such place, no mirror in this game. */
static inline bool plg_mirror_place(uint32_t plgSize, uint32_t over, uint32_t *ringOff)
{
    if (!over || (over & 0xFFFu) || over > PLG_RING_BYTES || (plgSize & 0xFFFu) || plgSize < PLG_RING_BYTES)
        return false;
    *ringOff = plgSize - PLG_RING_BYTES;
    return true;
}

/* The mirror mapped for this game (mirrorSize bytes of the ring at mirrorOff; 0: none) is this
   ring's: the ring starts where the mirror's bytes come from and ends where the mirror begins. */
static inline bool plg_mirror_serves(uint32_t mirrorSize, uint32_t mirrorOff, uint32_t ringOff, uint32_t ringSize,
                                     uint32_t plgSize, uint32_t over)
{
    return mirrorSize != 0 && mirrorSize == over && mirrorOff == ringOff && over <= ringSize &&
           ringOff + ringSize == plgSize;
}

#endif
