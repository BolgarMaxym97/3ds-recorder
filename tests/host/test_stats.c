/* "Share statistics" (app/source/stats_core.c): which log bytes are new, the state file, the
 * cleaning and the JSON. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stats_core.h"

static int failures;

#define CHECK(cond)                                                                                     \
    do {                                                                                                \
        if (!(cond)) {                                                                                  \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);                   \
            failures++;                                                                                 \
        }                                                                                               \
    } while (0)

static StatsFile File(const char *text)
{
    StatsFile f = {0};
    f.size = (uint32_t)strlen(text);
    f.headLen = f.size < STATS_HEAD ? f.size : STATS_HEAD;
    memcpy(f.head, text, f.headLen);
    return f;
}

static void test_id(void)
{
    char id[STATS_ID_LEN + 1];
    stats_id_encode((const uint8_t[]){0, 0, 0, 0, 0}, id);
    CHECK(strcmp(id, "00000000") == 0);
    stats_id_encode((const uint8_t[]){0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, id);
    CHECK(strcmp(id, "ZZZZZZZZ") == 0);
    CHECK(stats_id_valid("K7QM29XD"));
    CHECK(!stats_id_valid("K7QM29XI"));
    CHECK(!stats_id_valid("K7QM29X"));
    CHECK(!stats_id_valid(NULL));
}

static void test_state(void)
{
    StatsState s = {0};
    strcpy(s.id, "K7QM29XD");
    s.app = (StatsMark){.set = true, .sig = 0xDEADBEEF, .sigLen = 64, .off = 1234};
    s.dumpSet = true;
    s.dump = 7;
    char text[256];
    stats_state_format(&s, text, sizeof(text));
    CHECK(strcmp(text, "id=K7QM29XD\napp=deadbeef,64,1234\ndump=7\n") == 0);

    StatsState back;
    stats_state_parse(text, &back);
    CHECK(strcmp(back.id, "K7QM29XD") == 0);
    CHECK(back.app.set && back.app.sig == 0xDEADBEEF && back.app.sigLen == 64 && back.app.off == 1234);
    CHECK(!back.rec.set);
    CHECK(back.dumpSet && back.dump == 7);

    stats_state_parse("id=bad\r\napp=zz\nrec=1,99,5\ndump=x\njunk\n", &back);
    CHECK(!back.id[0] && !back.app.set && !back.rec.set && !back.dumpSet);
    stats_state_parse(NULL, &back);
    CHECK(!back.id[0]);
}

static void test_pick(void)
{
    const char *one = "[1] recorder started\n[2] a\n";
    const char *more = "[1] recorder started\n[2] a\n[3] b\n";
    StatsFile cur = File(more);

    /* the first time: the whole log when it fits */
    StatsMark none = {0};
    StatsPick p = stats_pick(&none, &cur, NULL, 1000);
    CHECK(p.curFrom == 0 && p.curTo == cur.size && p.oldTo == 0 && !p.trimmed);

    /* the first time with a long log: only its end */
    p = stats_pick(&none, &cur, NULL, 10);
    CHECK(p.curFrom == cur.size - 10 && p.trimmed);

    /* sent up to the end of "one": only "[3] b" is new */
    StatsFile was = File(one);
    StatsMark m = stats_mark(&was, was.size);
    p = stats_pick(&m, &cur, NULL, 1000);
    CHECK(p.curFrom == was.size && p.curTo == cur.size && !p.trimmed);

    /* nothing new */
    m = stats_mark(&cur, cur.size);
    p = stats_pick(&m, &cur, NULL, 1000);
    CHECK(p.curFrom == p.curTo);

    /* rotated: the mark is in recorder.old.log; its rest, then all of the new log */
    StatsFile old = cur;
    StatsFile fresh = File("[9] recorder started again\n");
    m = stats_mark(&was, was.size);
    p = stats_pick(&m, &fresh, &old, 1000);
    CHECK(p.oldFrom == was.size && p.oldTo == old.size && p.curFrom == 0 && p.curTo == fresh.size);

    /* rotated and too much: the old part goes first */
    p = stats_pick(&m, &fresh, &old, fresh.size + 2);
    CHECK(p.oldFrom == old.size - 2 && p.curFrom == 0 && p.trimmed);
    p = stats_pick(&m, &fresh, &old, 5);
    CHECK(p.oldFrom == p.oldTo && p.curFrom == fresh.size - 5 && p.trimmed);

    /* the file the mark is in shrank (rotated twice): treated as the first time */
    StatsFile big = File("[1] recorder started\n[2] a\n[3] b\n[4] c\n[5] d\n");
    m = stats_mark(&big, big.size);
    p = stats_pick(&m, &fresh, &old, 1000);
    CHECK(p.curFrom == 0 && p.oldTo == 0);

    /* marked while the log was still short: the sig covers what there was, and still matches */
    StatsFile tiny = File("[1] r");
    m = stats_mark(&tiny, tiny.size);
    p = stats_pick(&m, &cur, NULL, 1000);
    CHECK(p.curFrom == tiny.size);

    /* empty log marked: anything later is new */
    StatsFile empty = File("");
    m = stats_mark(&empty, 0);
    p = stats_pick(&m, &cur, NULL, 1000);
    CHECK(p.curFrom == 0 && p.curTo == cur.size);
}

static void test_next_mark(void)
{
    StatsFile was = File("[1] recorder started\n[2] a\n[3] b\n");
    StatsFile cur = File("[9] new\n");
    StatsFile empty = File("");
    const StatsMark m = stats_mark(&was, 10);

    /* the log has lines: the mark moves into it */
    StatsPick k = {.oldFrom = 10, .oldTo = was.size, .curFrom = 0, .curTo = cur.size};
    StatsMark n = stats_next_mark(&m, &cur, &was, &k, (was.size - 10) + cur.size, was.size - 10, cur.size);
    CHECK(n.set && n.off == cur.size && n.sigLen == cur.headLen);

    /* rotated and the new log is not there yet: the mark moves along the old file, so the same
       old lines do not go again next time */
    k = (StatsPick){.oldFrom = 10, .oldTo = was.size};
    n = stats_next_mark(&m, &empty, &was, &k, was.size - 10, was.size - 10, 0);
    CHECK(n.set && n.off == was.size && n.sig == m.sig);
    StatsPick again = stats_pick(&n, &empty, &was, 1000);
    CHECK(again.oldFrom == again.oldTo && again.curFrom == again.curTo);

    /* nothing in either: the mark stays */
    k = (StatsPick){0};
    n = stats_next_mark(&m, &empty, &empty, &k, 0, 0, 0);
    CHECK(n.off == m.off && n.sig == m.sig);
}

static void test_from(void)
{
    StatsState s = {0};
    char out[96];
    stats_from(&s, out, sizeof(out));
    CHECK(strcmp(out, "-/-") == 0);
    s.rec = (StatsMark){.set = true, .sig = 0xAB, .sigLen = 64, .off = 7};
    stats_from(&s, out, sizeof(out));
    CHECK(strcmp(out, "-/000000ab:64:7") == 0);
}

static void test_lines(void)
{
    size_t s, e, used;
    const char *buf = "tail of old\n[3] b\n[4] half";
    /* old part "tail of old\n" (12 bytes), the log "[3] b\n[4] half" */
    stats_lines(buf, strlen(buf), 12, false, &s, &e, &used);
    CHECK(s == 0 && e == 18 && used == 6);

    /* trimmed: from after the first newline */
    stats_lines(buf, strlen(buf), 12, true, &s, &e, &used);
    CHECK(s == 12 && e == 18 && used == 6);

    /* only half a line in the log: none of it is used */
    stats_lines("x\nhalf", 6, 2, false, &s, &e, &used);
    CHECK(s == 0 && e == 2 && used == 0);

    /* no newline at all */
    stats_lines("half", 4, 0, true, &s, &e, &used);
    CHECK(e - s == 0 && used == 0);
}

static void test_scrub(void)
{
    char text[] = "server running http://192.168.31.206:8080/?f=a.r3d, Luma 13.5.0, firm 2.58-0, 1.2.3.4.5, 10.0.0.1";
    const size_t n = stats_scrub(text, strlen(text));
    text[n] = 0;
    CHECK(strcmp(text, "server running http://x.x.x.x:8080/?f=a.r3d, Luma 13.5.0, firm 2.58-0, 1.2.3.4.5, x.x.x.x") == 0);
    char edge[] = "1234.1.1.1 and 1.1.1.1234";
    edge[stats_scrub(edge, strlen(edge))] = 0;
    CHECK(strcmp(edge, "1234.1.1.1 and 1.1.1.1234") == 0);
}

static void test_dumps(void)
{
    uint32_t n = 0;
    CHECK(stats_dump_number("crash_dump_00000012.dmp", &n) && n == 12);
    CHECK(!stats_dump_number("crash_dump_0000012.dmp", &n));
    CHECK(!stats_dump_number("crash_dump_0000001a.dmp", &n));
    CHECK(!stats_dump_number("crash_dump_00000012.bin", &n));

    const uint32_t nums[] = {3, 9, 4, 12};
    uint32_t pick = 0;
    CHECK(stats_next_dump(nums, 4, false, 0, &pick) && pick == 12);  /* first time: newest only */
    CHECK(stats_next_dump(nums, 4, true, 3, &pick) && pick == 4);    /* then the oldest not sent */
    CHECK(!stats_next_dump(nums, 4, true, 12, &pick));
    CHECK(!stats_next_dump(nums, 0, false, 0, &pick));
}

static void test_json(void)
{
    char out[128];
    StatsJson j;
    stats_json_begin(&j, out, sizeof(out));
    stats_json_int(&j, "v", 1);
    stats_json_str(&j, "s", "a\"b\\c\nd\te\x01Ї", strlen("a\"b\\c\nd\te\x01Ї"));
    CHECK(stats_json_end(&j));
    CHECK(strcmp(out, "{\"v\":1,\"s\":\"a\\\"b\\\\c\\nd\\te\\u0001Ї\"}") == 0);

    char small[8];
    stats_json_begin(&j, small, sizeof(small));
    stats_json_str(&j, "key", "value", 5);
    CHECK(!stats_json_end(&j));
}

int main(void)
{
    test_id();
    test_state();
    test_pick();
    test_next_mark();
    test_from();
    test_lines();
    test_scrub();
    test_dumps();
    test_json();
    if (failures) {
        fprintf(stderr, "test_stats: %d failed\n", failures);
        return 1;
    }
    printf("test_stats: ok\n");
    return 0;
}
