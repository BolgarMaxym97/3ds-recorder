#include "stats_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Crockford base32: no I, L, O, U (the same id format as 3ds-ua's installer). */
static const char ALPHABET[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

void stats_id_encode(const uint8_t bytes[5], char out[STATS_ID_LEN + 1])
{
    uint64_t v = 0;
    for (int i = 0; i < 5; i++)
        v = v << 8 | bytes[i];
    for (int i = STATS_ID_LEN - 1; i >= 0; i--, v >>= 5)
        out[i] = ALPHABET[v & 31];
    out[STATS_ID_LEN] = 0;
}

bool stats_id_valid(const char *id)
{
    if (!id || strlen(id) != STATS_ID_LEN)
        return false;
    for (int i = 0; i < STATS_ID_LEN; i++)
        if (!strchr(ALPHABET, id[i]))
            return false;
    return true;
}

static bool ParseMark(const char *v, StatsMark *m)
{
    unsigned long sig, len, off;
    if (sscanf(v, "%lx,%lu,%lu", &sig, &len, &off) != 3 || len > STATS_HEAD)
        return false;
    *m = (StatsMark){.set = true, .sig = (uint32_t)sig, .sigLen = (uint32_t)len, .off = (uint32_t)off};
    return true;
}

void stats_state_parse(const char *text, StatsState *s)
{
    memset(s, 0, sizeof(*s));
    while (text && *text) {
        const char *end = strchr(text, '\n');
        const size_t len = end ? (size_t)(end - text) : strlen(text);
        char line[96];
        if (len < sizeof(line)) {
            memcpy(line, text, len);
            line[len] = 0;
            line[strcspn(line, "\r")] = 0;
            if (!strncmp(line, "id=", 3) && stats_id_valid(line + 3))
                memcpy(s->id, line + 3, STATS_ID_LEN + 1);
            else if (!strncmp(line, "app=", 4))
                ParseMark(line + 4, &s->app);
            else if (!strncmp(line, "rec=", 4))
                ParseMark(line + 4, &s->rec);
            else if (!strncmp(line, "dump=", 5)) {
                char *e;
                const unsigned long n = strtoul(line + 5, &e, 10);
                if (e != line + 5 && !*e) {
                    s->dumpSet = true;
                    s->dump = (uint32_t)n;
                }
            }
        }
        text += len + (end ? 1 : 0);
    }
}

size_t stats_state_format(const StatsState *s, char *out, size_t cap)
{
    int n = snprintf(out, cap, "id=%s\n", s->id);
    const StatsMark *marks[2] = {&s->app, &s->rec};
    static const char *const keys[2] = {"app", "rec"};
    for (int i = 0; i < 2; i++)
        if (marks[i]->set && n >= 0 && (size_t)n < cap)
            n += snprintf(out + n, cap - (size_t)n, "%s=%08lx,%lu,%lu\n", keys[i], (unsigned long)marks[i]->sig,
                          (unsigned long)marks[i]->sigLen, (unsigned long)marks[i]->off);
    if (s->dumpSet && n >= 0 && (size_t)n < cap)
        n += snprintf(out + n, cap - (size_t)n, "dump=%lu\n", (unsigned long)s->dump);
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

static int FromMark(const StatsMark *m, char *out, size_t cap)
{
    return m->set ? snprintf(out, cap, "%08lx:%lu:%lu", (unsigned long)m->sig, (unsigned long)m->sigLen, (unsigned long)m->off)
                  : snprintf(out, cap, "-");
}

size_t stats_from(const StatsState *s, char *out, size_t cap)
{
    char app[40], rec[40];
    FromMark(&s->app, app, sizeof(app));
    FromMark(&s->rec, rec, sizeof(rec));
    const int n = snprintf(out, cap, "%s/%s", app, rec);
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

/* FNV-1a */
static uint32_t Hash(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

StatsMark stats_mark(const StatsFile *f, uint32_t off)
{
    return (StatsMark){.set = true, .sig = Hash(f->head, f->headLen), .sigLen = f->headLen, .off = off};
}

static bool InFile(const StatsMark *m, const StatsFile *f)
{
    return f->size >= m->off && f->headLen >= m->sigLen && Hash(f->head, m->sigLen) == m->sig;
}

StatsPick stats_pick(const StatsMark *m, const StatsFile *cur, const StatsFile *old, uint32_t cap)
{
    StatsPick p = {0};
    p.curTo = cur->size;
    if (m->set && InFile(m, cur))
        p.curFrom = m->off;
    else if (m->set && old && InFile(m, old)) { /* rotated since: the rest of the old file, then the new one */
        p.oldFrom = m->off;
        p.oldTo = old->size;
    } else if (cur->size > cap) { /* the first time: only the latest part */
        p.curFrom = cur->size - cap;
        p.trimmed = true;
    }
    uint32_t over = (p.oldTo - p.oldFrom) + (p.curTo - p.curFrom);
    if (over > cap) { /* the newest part fits; the older part counts as sent */
        over -= cap;
        const uint32_t fromOld = over < p.oldTo - p.oldFrom ? over : p.oldTo - p.oldFrom;
        p.oldFrom += fromOld;
        p.curFrom += over - fromOld;
        p.trimmed = true;
    }
    return p;
}

void stats_lines(const char *buf, size_t len, size_t oldLen, bool trimmed, size_t *start, size_t *end,
                 size_t *curUsed)
{
    size_t s = 0, e = len;
    if (trimmed) {
        const char *nl = memchr(buf, '\n', len);
        if (!nl) { /* all inside one long line: nothing whole yet, try again next time */
            *start = *end = *curUsed = 0;
            return;
        }
        s = (size_t)(nl - buf) + 1;
    }
    while (e > s && buf[e - 1] != '\n')
        e--;
    *start = s;
    *end = e;
    *curUsed = e > oldLen ? e - oldLen : 0;
}

StatsMark stats_next_mark(const StatsMark *m, const StatsFile *cur, const StatsFile *old, const StatsPick *k,
                          size_t end, size_t oldLen, size_t curUsed)
{
    if (cur->size)
        return stats_mark(cur, k->curFrom + (uint32_t)curUsed);
    if (old && k->oldTo > k->oldFrom)
        return stats_mark(old, k->oldFrom + (uint32_t)(end < oldLen ? end : oldLen));
    return *m;
}

static bool Digit(char c) { return c >= '0' && c <= '9'; }

/* An IPv4 address at s[i]: four groups of 1-3 digits, not part of a longer number or dotted run.
 * Returns its length, 0 if none. */
static size_t Address(const char *s, size_t len, size_t i)
{
    if (i > 0 && (Digit(s[i - 1]) || s[i - 1] == '.'))
        return 0;
    size_t j = i;
    for (int group = 0; group < 4; group++) {
        size_t d = 0;
        while (j < len && Digit(s[j]) && d < 4)
            j++, d++;
        if (d == 0 || d > 3)
            return 0;
        if (group < 3) {
            if (j >= len || s[j] != '.')
                return 0;
            j++;
        }
    }
    if (j < len && (Digit(s[j]) || (s[j] == '.' && j + 1 < len && Digit(s[j + 1]))))
        return 0;
    return j - i;
}

size_t stats_scrub(char *s, size_t len)
{
    size_t o = 0;
    for (size_t i = 0; i < len;) {
        const size_t n = Digit(s[i]) ? Address(s, len, i) : 0;
        if (n >= 7) {
            memcpy(s + o, "x.x.x.x", 7);
            o += 7;
            i += n;
        } else
            s[o++] = s[i++];
    }
    return o;
}

bool stats_dump_number(const char *name, uint32_t *n)
{
    static const char PREFIX[] = "crash_dump_";
    if (strlen(name) != sizeof(PREFIX) - 1 + 8 + 4 || strncmp(name, PREFIX, sizeof(PREFIX) - 1) ||
        strcmp(name + sizeof(PREFIX) - 1 + 8, ".dmp"))
        return false;
    uint32_t v = 0;
    for (const char *p = name + sizeof(PREFIX) - 1; p < name + sizeof(PREFIX) - 1 + 8; p++) {
        if (!Digit(*p))
            return false;
        v = v * 10 + (uint32_t)(*p - '0');
    }
    *n = v;
    return true;
}

bool stats_next_dump(const uint32_t *nums, size_t count, bool dumpSet, uint32_t last, uint32_t *out)
{
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (!dumpSet) {
            if (!found || nums[i] > *out)
                *out = nums[i], found = true;
        } else if (nums[i] > last && (!found || nums[i] < *out))
            *out = nums[i], found = true;
    }
    return found;
}

static void Put(StatsJson *j, const char *s, size_t n)
{
    if (j->full || j->len + n >= j->cap) {
        j->full = true;
        return;
    }
    memcpy(j->out + j->len, s, n);
    j->len += n;
    j->out[j->len] = 0;
}

static void Key(StatsJson *j, const char *key)
{
    Put(j, j->first ? "\"" : ",\"", j->first ? 1 : 2);
    j->first = false;
    Put(j, key, strlen(key));
    Put(j, "\":", 2);
}

void stats_json_begin(StatsJson *j, char *out, size_t cap)
{
    *j = (StatsJson){.out = out, .cap = cap, .first = true};
    Put(j, "{", 1);
}

void stats_json_str(StatsJson *j, const char *key, const char *value, size_t len)
{
    Key(j, key);
    Put(j, "\"", 1);
    size_t run = 0; /* bytes copied as they are, in one go */
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)value[i];
        if (c >= 0x20 && c != '"' && c != '\\') {
            run++;
            continue;
        }
        Put(j, value + i - run, run);
        run = 0;
        char esc[8];
        const char *e = c == '"' ? "\\\"" : c == '\\' ? "\\\\" : c == '\n' ? "\\n" : c == '\r' ? "\\r" : c == '\t' ? "\\t" : NULL;
        if (!e) {
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            e = esc;
        }
        Put(j, e, strlen(e));
    }
    Put(j, value + len - run, run);
    Put(j, "\"", 1);
}

void stats_json_int(StatsJson *j, const char *key, long value)
{
    char num[24];
    Key(j, key);
    Put(j, num, (size_t)snprintf(num, sizeof(num), "%ld", value));
}

bool stats_json_end(StatsJson *j)
{
    Put(j, "}", 1);
    return !j->full;
}
