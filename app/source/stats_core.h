/*
 * "Share statistics": the parts that do not touch the console, so tests/host can run them.
 * stats.c reads the files and posts; this picks which log bytes are new, keeps the state file's
 * text, cleans the lines and builds the JSON the Worker (stats/src/report.ts) takes.
 *
 * What was sent is remembered per log as a mark: how far it was sent (off) and a hash of the
 * file's first bytes (sig), which tells the same file from the next one after a rotation
 * (recorder.log -> recorder.old.log when it passes 256 KB).
 */
#ifndef STATS_CORE_H
#define STATS_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STATS_ID_LEN 8      /* 40 bits as Crockford base32 */
#define STATS_HEAD 64       /* bytes hashed for a log's sig: the first line, with its time stamp */

typedef struct {
    bool set;               /* false: nothing was sent from this log yet */
    uint32_t sig, sigLen;   /* hash of the first sigLen bytes of the file the mark is in */
    uint32_t off;           /* sent up to here */
} StatsMark;

typedef struct {
    char id[STATS_ID_LEN + 1]; /* "" until made */
    StatsMark app, rec;        /* app.log, recorder.log */
    bool dumpSet;              /* false: no dump looked at yet */
    uint32_t dump;             /* number of the last crash dump sent (or skipped) */
} StatsState;

/* A log file as found: its size and first bytes. size 0 = missing or empty. */
typedef struct {
    uint32_t size;
    uint32_t headLen;          /* min(size, STATS_HEAD) */
    uint8_t head[STATS_HEAD];
} StatsFile;

/* Which bytes to send: [oldFrom, oldTo) of the *.old.log, then [curFrom, curTo) of the log. */
typedef struct {
    uint32_t oldFrom, oldTo, curFrom, curTo;
    bool trimmed;              /* the start was cut to fit the cap: it is mid-line */
} StatsPick;

void stats_id_encode(const uint8_t bytes[5], char out[STATS_ID_LEN + 1]);
bool stats_id_valid(const char *id);

/* "id=..\napp=sig,len,off\nrec=..\ndump=N\n" <-> StatsState. Unknown or bad lines are skipped. */
void stats_state_parse(const char *text, StatsState *s);
size_t stats_state_format(const StatsState *s, char *out, size_t cap);

/* The marks a report is sent from, "app/rec" as sig:len:off ("-" when unset): the Worker merges a
 * report sent again from the same marks (its 2xx got lost) into the row it repeats. */
size_t stats_from(const StatsState *s, char *out, size_t cap);

/* The mark for "sent up to off" in this file. */
StatsMark stats_mark(const StatsFile *f, uint32_t off);

/* New bytes since the mark, newest `cap` of them. No mark: the last `cap` bytes of the log. */
StatsPick stats_pick(const StatsMark *m, const StatsFile *cur, const StatsFile *old, uint32_t cap);

/* buf holds the picked old bytes (oldLen of them) and then the log's. Cuts to whole lines: from
 * after the first newline when the pick was trimmed, up to the last newline (a line being written
 * waits for the next launch). Gives [*start, *end) in buf and how many of the log's bytes that
 * used (from curFrom; the new mark's off is curFrom + *curUsed). */
void stats_lines(const char *buf, size_t len, size_t oldLen, bool trimmed, size_t *start, size_t *end,
                 size_t *curUsed);

/* The mark after [start, end) of that buffer was sent: in the log at curFrom + curUsed; while the
 * log is still empty or missing after a rotation, in the old file after what of it went; with
 * neither, the mark stays (an empty file's mark would match any later file). */
StatsMark stats_next_mark(const StatsMark *m, const StatsFile *cur, const StatsFile *old, const StatsPick *k,
                          size_t end, size_t oldLen, size_t curUsed);

/* Replaces IPv4 addresses (the phone page's LAN address in app.log) with x.x.x.x, in place.
 * Returns the new length. */
size_t stats_scrub(char *s, size_t len);

/* crash_dump_00000012.dmp -> 12. */
bool stats_dump_number(const char *name, uint32_t *n);
/* The dump to send of the `count` numbers found: the lowest after the last one sent, or, the
 * first time, only the newest (older dumps are from before the app shared anything).
 * false: none. */
bool stats_next_dump(const uint32_t *nums, size_t count, bool dumpSet, uint32_t last, uint32_t *out);

/* JSON text built into a fixed buffer; full is set (and the text is no good) when it overflows. */
typedef struct {
    char *out;
    size_t cap, len;
    bool full, first;
} StatsJson;

void stats_json_begin(StatsJson *j, char *out, size_t cap);
void stats_json_str(StatsJson *j, const char *key, const char *value, size_t len);
void stats_json_int(StatsJson *j, const char *key, long value);
bool stats_json_end(StatsJson *j);

#endif
