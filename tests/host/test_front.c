/* A program that has just started is left alone (sysmod/source/front.h): when the replay buffer
   may start and the processor's speed may be changed or put back. */
#include "front.h"

#include <stdio.h>

static int g_fail;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            g_fail = 1;                                                  \
        }                                                                \
    } while (0)

#define Q 15u   /* the quiet time, in the tests' unit */

int main(void)
{
    /* a program that was there when the module first looked: age unknown, nothing held back */
    {
        FrontApp f = { 0 };
        CHECK(!front_quiet(&f, 0, Q));
        CHECK(!front_note(&f, 44, 100));
        CHECK(!front_quiet(&f, 100, Q));
        CHECK(!front_note(&f, 44, 102));
        CHECK(!front_quiet(&f, 102, Q));
    }
    /* HOME alone first, then a game starts: quiet for Q, then not */
    {
        FrontApp f = { 0 };
        CHECK(!front_note(&f, 0, 100));
        CHECK(!front_quiet(&f, 100, Q));
        CHECK(front_note(&f, 45, 110));
        CHECK(front_quiet(&f, 110, Q));
        CHECK(front_quiet(&f, 124, Q));
        CHECK(!front_quiet(&f, 125, Q));
        CHECK(!front_quiet(&f, 100000, Q));
    }
    /* the first program was old; it closes and the next one starts: that one is new */
    {
        FrontApp f = { 0 };
        front_note(&f, 44, 100);
        CHECK(!front_note(&f, 0, 200));
        CHECK(!front_quiet(&f, 200, Q));
        CHECK(front_note(&f, 46, 210));
        CHECK(front_quiet(&f, 211, Q));
        CHECK(!front_quiet(&f, 226, Q));
    }
    /* the process list fails for a moment (pid 0), the same program is back: not a new start */
    {
        FrontApp f = { 0 };
        front_note(&f, 0, 0);
        front_note(&f, 45, 10);
        CHECK(!front_note(&f, 0, 500));
        CHECK(!front_note(&f, 45, 502));
        CHECK(!front_quiet(&f, 502, Q));
    }
    /* one program straight after another, never "none" in between */
    {
        FrontApp f = { 0 };
        front_note(&f, 44, 0);
        CHECK(front_note(&f, 45, 300));
        CHECK(front_quiet(&f, 305, Q));
    }
    /* pm's 0x10C alone (the module does not look at pids when idle with Replay off) */
    {
        FrontApp f = { 0 };
        CHECK(!front_quiet(&f, 1000, Q));
        front_started(&f, 1000);
        CHECK(front_quiet(&f, 1000, Q));
        CHECK(front_quiet(&f, 1014, Q));
        CHECK(!front_quiet(&f, 1015, Q));
        /* and with the pid seen a little later: the later of the two counts */
        front_note(&f, 0, 1001);
        front_note(&f, 47, 1003);
        CHECK(front_quiet(&f, 1016, Q));
        CHECK(!front_quiet(&f, 1018, Q));
    }
    /* the processor's speed goes back only for the same program, and only from the recorder's state */
    CHECK(cpu_restore_ok(44, 44, 3));
    CHECK(cpu_restore_ok(0, 0, 3));     /* boosted in HOME alone, still HOME alone */
    CHECK(!cpu_restore_ok(44, 45, 3));  /* another program now */
    CHECK(!cpu_restore_ok(44, 0, 3));   /* the program closed */
    CHECK(!cpu_restore_ok(0, 44, 3));   /* a program started since */
    CHECK(!cpu_restore_ok(44, 44, 0));  /* NS changed the speed itself */
    CHECK(!cpu_restore_ok(44, 44, 1));
    CHECK(!cpu_restore_ok(44, 44, 2));

    if (!g_fail)
        printf("test_front: ok\n");
    return g_fail;
}
