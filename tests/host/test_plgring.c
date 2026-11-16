/* The ring in the launcher plugin's memory and its mirror (sysmod/source/plgring.h): one place for
   the ring in a game, whatever is recorded, so the mirror mapped once serves every recording. */
#include "plgring.h"

#include <stdio.h>

static int g_fail;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            g_fail = 1;                                                  \
        }                                                                \
    } while (0)

#define KB    1024u
#define BLOCK (128u * KB)   /* BLOCK_LITE */
#define PLG   (2044u * KB)  /* the launcher plugin's heap */
#define OVER  (188u * KB)   /* one whole record, in pages */

int main(void)
{
    CHECK(PLG_RING_BYTES % BLOCK == 0);

    /* the replay buffer left 1024-1151 KB, a recording 768-895 KB (O3DS 1.2.0): the same ring */
    CHECK(plg_ring_size(1024u * KB, BLOCK) == PLG_RING_BYTES);
    CHECK(plg_ring_size(1151u * KB, BLOCK) == PLG_RING_BYTES);
    CHECK(plg_ring_size(768u * KB, BLOCK) == PLG_RING_BYTES);
    CHECK(plg_ring_size(895u * KB, BLOCK) == PLG_RING_BYTES);
    CHECK(plg_ring_size(PLG, BLOCK) == PLG_RING_BYTES);
    /* less left (more buffers by a setting): whole blocks, as before */
    CHECK(plg_ring_size(767u * KB, BLOCK) == 640u * KB);
    CHECK(plg_ring_size(600u * KB, BLOCK) == 512u * KB);
    CHECK(plg_ring_size(100u * KB, BLOCK) == 0);

    /* the mirror's place: the last PLG_RING_BYTES of the plugin's memory */
    uint32_t off = 0;
    CHECK(plg_mirror_place(PLG, OVER, &off) && off == 1276u * KB);
    CHECK(plg_mirror_place(PLG_RING_BYTES, OVER, &off) && off == 0);
    CHECK(!plg_mirror_place(PLG_RING_BYTES - 4u * KB, OVER, &off));   /* too small for the ring */
    CHECK(!plg_mirror_place(PLG, 0, &off));
    CHECK(!plg_mirror_place(PLG, OVER + 1u, &off));                   /* not whole pages */
    CHECK(!plg_mirror_place(PLG + 1u, OVER, &off));
    CHECK(!plg_mirror_place(PLG, PLG_RING_BYTES + 4u * KB, &off));    /* more than the ring */

    /* the buffer and a recording, in any order, both get the mirror mapped at the game's start */
    CHECK(plg_mirror_place(PLG, OVER, &off));
    for (uint32_t left = 768u * KB; left <= 1280u * KB; left += 4u * KB) {
        const uint32_t size = plg_ring_size(left, BLOCK);
        CHECK(plg_mirror_serves(OVER, off, PLG - size, size, PLG, OVER));
    }
    /* a smaller ring starts elsewhere: pad records, the mirror is not used */
    {
        const uint32_t size = plg_ring_size(700u * KB, BLOCK);
        CHECK(!plg_mirror_serves(OVER, off, PLG - size, size, PLG, OVER));
    }
    CHECK(!plg_mirror_serves(0, 0, off, PLG_RING_BYTES, PLG, OVER));                       /* none mapped */
    CHECK(!plg_mirror_serves(OVER, off, off, PLG_RING_BYTES, PLG, OVER + 4u * KB));        /* another size asked */
    CHECK(!plg_mirror_serves(OVER, off, off, PLG_RING_BYTES - BLOCK, PLG, OVER));          /* ends before the mirror */
    CHECK(!plg_mirror_serves(PLG_RING_BYTES, off, off, PLG_RING_BYTES - BLOCK, PLG, PLG_RING_BYTES)); /* over > ring */

    if (!g_fail)
        printf("test_plgring: ok\n");
    return g_fail;
}
