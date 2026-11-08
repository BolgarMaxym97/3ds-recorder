#include "server.h"
#include "prepare.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "gallery.h"
#include "recorder.h"

#define SOC_SIZE 0x100000u          /* SOC service buffer (0x1000 aligned) */
#define CHUNK (64u << 10)           /* SD read and send() size; 4-16 KB sends measured the same */
#define PAGE_PATH "romfs:/web/index.html.gz"

/* C_DRAIN: the answer is sent and the write side shut; reading until the phone closes, so the
   end of the answer is not cut off by a reset (closing with unread input resets the link) */
typedef enum { C_FREE, C_READ, C_SEND, C_DRAIN } CState;
typedef struct {
    CState state;
    int fd;
    u64 since;            /* osGetTime() when the state began (idle connections are dropped) */
    char req[2048];                 /* /api/log lines from the page ride in the request line */
    int reqLen;
    char head[512];
    int headLen, headPos;
    Handle file;                    /* 0: body from memory */
    u64 off, left;
    const u8 *body;                 /* page / JSON */
    u8 *ownBody;                    /* freed with the request */
    u8 *buf;
    u32 bufLen, bufPos;
    char name[32];                  /* the recording being sent */
    u64 sent, t0;                   /* bytes of it sent, osGetTime() of the request (app.log) */
    u64 lastSend;                   /* osGetTime() of the last byte that went out */
    int err;                        /* errno of a failed send, for the log */
} Client;

static Thread g_thread;
static volatile bool g_quit;
static LightLock g_lock;
static ServerStats g_stats;         /* under g_lock */
static u32 *g_socBuf;
static int g_srv = -1;
static Client g_cl[SRV_CLIENTS];
static u8 *g_page;
static u32 g_pageLen;
static char g_info[160];           /* "lang":..,"version":..,"console":.. (into /api/rec) */
static char g_offer[32];           /* the one recording served */
/* speed: bytes sent per 250 ms slot, over the last 3 s */
static u32 g_slots[12];
static u64 g_slotAt;

/* ------------------------------------------------------------------ helpers */

static bool valid_name(const char *n)
{
    /* yyyymmdd_hhmmss.r3d (or .mp4, made from it) and nothing else: no paths */
    if (strlen(n) != 19 || (strcmp(n + 15, ".r3d") != 0 && strcmp(n + 15, ".mp4") != 0) || n[8] != '_')
        return false;
    for (int i = 0; i < 15; i++)
        if (i != 8 && (n[i] < '0' || n[i] > '9'))
            return false;
    return true;
}

static void count_sent(Client *c, u32 n)
{
    const u64 now = osGetTime();
    LightLock_Lock(&g_lock);
    const u64 slot = now / 250;
    if (slot - g_slotAt >= 12) { /* first byte, or idle for 3 s: start the window over */
        memset(g_slots, 0, sizeof(g_slots));
        g_slotAt = slot;
    }
    while (g_slotAt < slot) { /* at most 11 steps */
        g_slotAt++;
        g_slots[g_slotAt % 12] = 0;
    }
    g_slots[slot % 12] += n;
    if (c->name[0] && strcmp(c->name, g_stats.file) == 0)
        g_stats.fileDone += n;
    g_stats.lastActivityMs = (u32)now;
    LightLock_Unlock(&g_lock);
}

static void set_head(Client *c, const char *status, const char *type, u64 len, const char *extra)
{
    c->headLen = snprintf(c->head, sizeof(c->head),
                          "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %llu\r\nConnection: close\r\n"
                          "Cache-Control: no-store\r\n%s\r\n",
                          status, type, (unsigned long long)len, extra ? extra : "");
    c->headPos = 0;
}

static void send_text(Client *c, const char *status, const char *type, char *text)
{
    c->ownBody = (u8 *)text;
    c->body = c->ownBody;
    c->left = strlen(text);
    set_head(c, status, type, c->left, NULL);
}

#define IDLE_MS 8000    /* a connection that sends no request (Safari opens some in advance) */
#define DRAIN_MS 3000   /* waiting for the phone to close after the answer */
#define STALL_MS 25000  /* an answer nobody reads any more: the phone gave up on it (weak signal) */
#define STALE_MS 5000   /* when every connection is taken, one stuck this long makes room */

static void close_client(Client *c)
{
    if (c->name[0] && c->left) /* a file answer that did not get out whole */
        app_log("srv: %s cut after %llu bytes, %llu left, %llu ms (errno %d)", c->name, (unsigned long long)c->sent,
                (unsigned long long)c->left, (unsigned long long)(osGetTime() - c->t0), c->err);
    c->name[0] = 0;
    if (c->file)
        FSFILE_Close(c->file);
    c->file = 0;
    free(c->ownBody);
    c->ownBody = NULL;
    c->body = NULL;
    close(c->fd);
    c->state = C_FREE;
}

/* The answer is out: stop sending, let the phone read the rest and close first. */
static void finish_client(Client *c)
{
    /* one line per file, not per piece (the page asks for 512 KB at a time: a long recording
       made a thousand lines); failures are logged in close_client, the page logs the total */
    if (c->name[0] && c->off >= g_stats.fileSize && g_stats.fileSize)
        app_log("srv: %s last piece sent", c->name);
    c->name[0] = 0;
    if (c->file)
        FSFILE_Close(c->file);
    c->file = 0;
    free(c->ownBody);
    c->ownBody = NULL;
    c->body = NULL;
    shutdown(c->fd, SHUT_WR);
    c->state = C_DRAIN;
    c->since = osGetTime();
}

/* ------------------------------------------------------------------ routes */

/* The offered recording's details (the page shows them and needs size and length). */
static char *rec_json(void)
{
    GalItem it;
    memset(&it, 0, sizeof(it));
    snprintf(it.name, sizeof(it.name), "%.31s", g_offer);
    char path[96];
    snprintf(path, sizeof(path), REC_FILES_FS "/%.31s", g_offer);
    Handle f = 0;
    if (R_SUCCEEDED(FSUSER_OpenFile(&f, recorder_archive(), fsMakePath(PATH_ASCII, path), FS_OPEN_READ, 0))) {
        FSFILE_GetSize(f, &it.bytes);
        FSFILE_Close(f);
    }
    gal_read_info(&it);
    /* the MP4 the console prepares from it (prepare.c) */
    static const char *const prepName[] = {"none", "queued", "working", "ready", "failed"};
    int permille = -1;
    const PrepareState ps = prepare_state(g_offer, &permille);
    u64 mp4 = 0;
    if (ps == PS_READY) {
        char mp4Path[96];
        snprintf(mp4Path, sizeof(mp4Path), REC_FILES_FS "/%.15s.mp4", g_offer);
        if (R_SUCCEEDED(FSUSER_OpenFile(&f, recorder_archive(), fsMakePath(PATH_ASCII, mp4Path), FS_OPEN_READ, 0))) {
            FSFILE_GetSize(f, &mp4);
            FSFILE_Close(f);
        }
    }
    char *out = malloc(640);
    if (out)
        snprintf(out, 640,
                 "{\"name\":\"%s\",\"size\":%llu,\"ms\":%lu,\"closed\":%s,\"both\":%s,\"audio\":%s,\"fps\":%u,"
                 "\"ok\":%s,\"prep\":\"%s\",\"prepPct\":%d,\"mp4\":%llu,%s}",
                 it.name, (unsigned long long)it.bytes, (unsigned long)it.durationMs, it.ended ? "true" : "false",
                 it.both ? "true" : "false", it.audio ? "true" : "false", (unsigned)it.fps,
                 it.readable && it.bytes ? "true" : "false", prepName[ps], permille < 0 ? 0 : permille,
                 (unsigned long long)mp4, g_info);
    return out;
}

/* The value of a request header (name in lower case), or NULL. */
static const char *header_value(const char *req, const char *name)
{
    const size_t n = strlen(name);
    for (const char *p = strstr(req, "\r\n"); p; p = strstr(p + 2, "\r\n")) {
        const char *h = p + 2;
        if (strncasecmp(h, name, n) == 0 && h[n] == ':') {
            h += n + 1;
            while (*h == ' ')
                h++;
            return h;
        }
    }
    return NULL;
}

/* Range: bytes=a-b / a- / -n, against a file of `size` bytes; false: not satisfiable. */
static bool parse_range(const char *req, u64 size, u64 *from, u64 *to, bool *partial)
{
    *partial = false;
    *from = 0;
    *to = size ? size - 1 : 0;
    const char *r = header_value(req, "range");
    if (!r || strncasecmp(r, "bytes=", 6) != 0)
        return true;
    r += 6;
    char *end;
    if (*r == '-') { /* the last n bytes */
        const u64 n = strtoull(r + 1, &end, 10);
        if (!n || !size)
            return false;
        *from = n > size ? 0 : size - n;
    } else {
        *from = strtoull(r, &end, 10);
        if (*end == '-' && end[1] >= '0' && end[1] <= '9')
            *to = strtoull(end + 1, NULL, 10);
    }
    if (*to >= size)
        *to = size - 1;
    if (*from > *to || *from >= size)
        return false;
    *partial = true;
    return true;
}

static void serve_recording(Client *c, const char *name)
{
    char path[96];
    snprintf(path, sizeof(path), REC_FILES_FS "/%.31s", name);
    u64 size = 0;
    const bool mp4 = valid_name(name) && strcmp(name + 15, ".mp4") == 0;
    const bool offered = mp4 ? strncmp(name, g_offer, 15) == 0 && prepare_state(g_offer, NULL) == PS_READY
                             : strcmp(name, g_offer) == 0;
    if (!valid_name(name) || !offered ||
        R_FAILED(FSUSER_OpenFile(&c->file, recorder_archive(), fsMakePath(PATH_ASCII, path), FS_OPEN_READ, 0))) {
        c->file = 0;
        send_text(c, "404 Not Found", "text/plain", strdup("not found"));
        return;
    }
    FSFILE_GetSize(c->file, &size);
    u64 from, to;
    bool partial;
    if (!parse_range(c->req, size, &from, &to, &partial)) {
        char extra[64];
        snprintf(extra, sizeof(extra), "Content-Range: bytes */%llu\r\n", (unsigned long long)size);
        FSFILE_Close(c->file);
        c->file = 0;
        c->body = (const u8 *)"";
        c->left = 0;
        set_head(c, "416 Range Not Satisfiable", "text/plain", 0, extra);
        return;
    }
    c->off = from;
    c->left = size ? to - from + 1 : 0;
    c->bufLen = c->bufPos = 0;
    snprintf(c->name, sizeof(c->name), "%.31s", name);
    c->sent = 0;
    c->err = 0;
    c->t0 = c->lastSend = osGetTime();
    if (from == 0) /* the first piece only */
        app_log("srv: GET %s (%llu bytes)%s", name, (unsigned long long)size, partial ? "" : " whole");
    char extra[192];
    if (partial)
        snprintf(extra, sizeof(extra), "Accept-Ranges: bytes\r\nContent-Range: bytes %llu-%llu/%llu\r\n",
                 (unsigned long long)from, (unsigned long long)to, (unsigned long long)size);
    else
        snprintf(extra, sizeof(extra), "Accept-Ranges: bytes\r\nContent-Disposition: attachment; filename=\"%.31s\"\r\n",
                 name);
    set_head(c, partial ? "206 Partial Content" : "200 OK", mp4 ? "video/mp4" : "application/octet-stream", c->left, extra);
    LightLock_Lock(&g_lock);
    if (strcmp(g_stats.file, name) != 0) { /* a new recording: its progress starts over */
        snprintf(g_stats.file, sizeof(g_stats.file), "%.31s", name);
        g_stats.fileDone = 0;
    }
    g_stats.fileSize = size;
    LightLock_Unlock(&g_lock);
}

static void page_log(const char *query);
static void serve_diagnostics(Client *c);

static void start_request(Client *c)
{
    char method[8] = "", path[1024] = "/";
    sscanf(c->req, "%7s %1023s", method, path);
    c->file = 0;
    c->body = NULL;
    c->ownBody = NULL;
    c->name[0] = 0;
    c->state = C_SEND;
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        send_text(c, "405 Method Not Allowed", "text/plain", strdup("GET only"));
        return;
    }
    char *q = strchr(path, '?');
    if (q)
        *q++ = 0;
    LightLock_Lock(&g_lock); /* any request: the phone is there (it asks for progress every second) */
    g_stats.lastActivityMs = (u32)osGetTime();
    LightLock_Unlock(&g_lock);
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        if (!g_page) {
            send_text(c, "500 Internal Server Error", "text/plain", strdup("page missing"));
            return;
        }
        c->body = g_page;
        c->left = g_pageLen;
        set_head(c, "200 OK", "text/html; charset=utf-8", g_pageLen, "Content-Encoding: gzip\r\n");
    } else if (strcmp(path, "/api/rec") == 0) {
        char *json = rec_json();
        send_text(c, json ? "200 OK" : "500 Internal Server Error", "application/json", json ? json : strdup("{}"));
    } else if (strcmp(path, "/api/log") == 0) {
        page_log(q);
        send_text(c, "200 OK", "text/plain", strdup("ok"));
    } else if (strcmp(path, "/log.txt") == 0)
        serve_diagnostics(c);
    else if (strncmp(path, "/rec/", 5) == 0)
        serve_recording(c, path + 5);
    else
        send_text(c, "404 Not Found", "text/plain", strdup("not found"));
    if (strcmp(method, "HEAD") == 0) { /* the head only */
        if (c->file)
            FSFILE_Close(c->file);
        c->file = 0;
        c->left = 0;
    }
}

/* ------------------------------------------------------------------ diagnostics (remote testing) */

/* A line from the page about what the phone did: GET /api/log?m=<url-encoded text>. */
static void page_log(const char *query)
{
    char msg[600];
    size_t n = 0;
    const char *m = query && strncmp(query, "m=", 2) == 0 ? query + 2 : NULL;
    for (; m && *m && *m != '&' && n + 1 < sizeof(msg); m++) {
        unsigned char ch = (unsigned char)*m;
        if (ch == '+')
            ch = ' ';
        else if (ch == '%' && isxdigit((unsigned char)m[1]) && isxdigit((unsigned char)m[2])) {
            const char hex[3] = {m[1], m[2], 0};
            ch = (unsigned char)strtoul(hex, NULL, 16);
            m += 2;
        }
        msg[n++] = ch < 0x20 ? ' ' : (char)ch;
    }
    msg[n] = 0;
    app_log("page: %s", msg);
}

typedef struct {
    char *p;
    size_t len, cap;
} Text;

static void add(Text *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void add(Text *t, const char *fmt, ...)
{
    if (t->len + 1 >= t->cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(t->p + t->len, t->cap - t->len, fmt, ap);
    va_end(ap);
    if (n > 0)
        t->len = t->len + (size_t)n < t->cap ? t->len + (size_t)n : t->cap - 1;
}

/* The last `tail` bytes of a text file. */
static void add_file(Text *t, const char *title, const char *path, long tail)
{
    add(t, "\n===== %s =====\n", title);
    FILE *f = fopen(path, "rb");
    if (!f) {
        add(t, "(none)\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f), from = size > tail ? size - tail : 0;
    fseek(f, from, SEEK_SET);
    if (from)
        add(t, "(the first %ld bytes left out)\n", from);
    const size_t room = t->cap - 1 - t->len, want = (size_t)(size - from);
    t->len += fread(t->p + t->len, 1, want < room ? want : room, f);
    t->p[t->len] = 0;
    fclose(f);
}

static void add_dir(Text *t, const char *title, const char *path)
{
    add(t, "\n===== %s =====\n", title);
    DIR *d = opendir(path);
    if (!d) {
        add(t, "(none)\n");
        return;
    }
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        char full[320];
        struct stat st;
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        if (stat(full, &st) == 0)
            add(t, "%-28s %12lld\n", e->d_name, (long long)st.st_size);
        else
            add(t, "%s\n", e->d_name);
    }
    closedir(d);
}

/* GET /log.txt: everything needed to understand a test from afar, in one file the phone saves
   and sends (the console line, settings, files, crash dumps, both logs). */
static void serve_diagnostics(Client *c)
{
    Text t = {malloc(256 * 1024), 0, 256 * 1024};
    if (!t.p) {
        send_text(c, "500 Internal Server Error", "text/plain", strdup("out of memory"));
        return;
    }
    t.p[0] = 0;
    const time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    const char *ua = header_value(c->req, "user-agent");
    const char *uaEnd = ua ? strstr(ua, "\r\n") : NULL;
    add(&t, "3DS Recorder diagnostics, %04d-%02d-%02d %02d:%02d:%02d\n%s\nphone: %.*s\n", tm.tm_year + 1900,
        tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, app_about(),
        ua ? (int)(uaEnd ? uaEnd - ua : (long)strlen(ua)) : 0, ua ? ua : "");
    add_file(&t, "config.ini", "sdmc:/3ds/recorder/config.ini", 4096);
    add_dir(&t, "recordings", "sdmc:/3ds/recorder/recordings");
    add_dir(&t, "recorder folder", "sdmc:/3ds/recorder");
    add_dir(&t, "Luma crash dumps (arm11)", "sdmc:/luma/dumps/arm11");
    add_file(&t, "app.log", REC_APP_LOG, 100 * 1024);
    add_file(&t, "recorder.log", "sdmc:/3ds/recorder/recorder.log", 100 * 1024);
    add_file(&t, "recorder.old.log", "sdmc:/3ds/recorder/recorder.old.log", 16 * 1024);
    app_log("srv: diagnostics, %u bytes", (unsigned)t.len);
    c->ownBody = (u8 *)t.p;
    c->body = c->ownBody;
    c->left = t.len;
    char extra[128];
    snprintf(extra, sizeof(extra), "Content-Disposition: attachment; filename=\"3ds-recorder-log-%04d%02d%02d-%02d%02d.txt\"\r\n",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    set_head(c, "200 OK", "text/plain; charset=utf-8", t.len, extra);
}

/* ------------------------------------------------------------------ sockets */

static void on_read(Client *c)
{
    char sink[256];
    const ssize_t r = c->state == C_DRAIN ? recv(c->fd, sink, sizeof(sink), 0)
                                          : recv(c->fd, c->req + c->reqLen, sizeof(c->req) - 1 - c->reqLen, 0);
    if (r <= 0) {
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        close_client(c);
        return;
    }
    if (c->state == C_DRAIN) /* whatever the phone still sends: dropped */
        return;
    c->reqLen += (int)r;
    c->req[c->reqLen] = 0;
    if (strstr(c->req, "\r\n\r\n") || c->reqLen >= (int)sizeof(c->req) - 1)
        start_request(c);
}

static void on_write(Client *c)
{
    if (c->headPos < c->headLen) {
        const ssize_t w = send(c->fd, c->head + c->headPos, c->headLen - c->headPos, 0);
        if (w < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                close_client(c);
            return;
        }
        c->headPos += (int)w;
        return;
    }
    if (!c->left) {
        finish_client(c);
        return;
    }
    const u8 *p;
    u32 n;
    if (c->file) {
        if (c->bufPos == c->bufLen) {
            const u32 want = c->left < CHUNK ? (u32)c->left : CHUNK;
            u32 got = 0;
            if (R_FAILED(FSFILE_Read(c->file, &got, c->off, c->buf, want)) || !got) {
                close_client(c);
                return;
            }
            c->off += got;
            c->bufLen = got;
            c->bufPos = 0;
        }
        p = c->buf + c->bufPos;
        n = c->bufLen - c->bufPos;
    } else {
        p = c->body;
        n = c->left > CHUNK ? CHUNK : (u32)c->left;
    }
    const ssize_t w = send(c->fd, p, n, 0);
    if (w < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            c->err = errno;
            close_client(c);
        }
        return;
    }
    c->left -= (u64)w;
    c->sent += (u64)w;
    if (w > 0)
        c->lastSend = osGetTime();
    if (c->file) {
        c->bufPos += (u32)w;
        count_sent(c, (u32)w);
    } else
        c->body += w;
}

static void thread_main(void *arg)
{
    (void)arg;
    while (!g_quit) {
        struct pollfd pf[SRV_CLIENTS + 1];
        int idx[SRV_CLIENTS + 1], n = 0, busy = 0;
        pf[n].fd = g_srv;
        pf[n].events = POLLIN;
        idx[n++] = -1;
        for (int i = 0; i < SRV_CLIENTS; i++)
            if (g_cl[i].state != C_FREE) {
                pf[n].fd = g_cl[i].fd;
                pf[n].events = g_cl[i].state == C_SEND ? POLLOUT : POLLIN;
                idx[n++] = i;
                busy += g_cl[i].state == C_SEND || (g_cl[i].state == C_READ && g_cl[i].reqLen > 0);
            }
        LightLock_Lock(&g_lock);
        g_stats.connections = busy;
        LightLock_Unlock(&g_lock);
        const u64 now = osGetTime();
        for (int i = 0; i < SRV_CLIENTS; i++) /* idle and finished connections go */
            if ((g_cl[i].state == C_READ && now - g_cl[i].since > IDLE_MS) ||
                (g_cl[i].state == C_DRAIN && now - g_cl[i].since > DRAIN_MS) ||
                (g_cl[i].state == C_SEND && g_cl[i].file && now - g_cl[i].lastSend > STALL_MS)) {
                if (g_cl[i].state == C_SEND)
                    g_cl[i].err = -1000; /* stalled */
                close_client(&g_cl[i]);
            }
        if (poll(pf, n, 50) <= 0)
            continue;
        for (int k = 0; k < n; k++) {
            if (!pf[k].revents)
                continue;
            if (idx[k] < 0) {
                const int fd = accept(g_srv, NULL, NULL);
                if (fd < 0)
                    continue;
                Client *c = NULL;
                for (int i = 0; i < SRV_CLIENTS && !c; i++)
                    if (g_cl[i].state == C_FREE)
                        c = &g_cl[i];
                /* full: a finished one, else the oldest that has sent nothing, makes room */
                for (int i = 0; i < SRV_CLIENTS && !c; i++)
                    if (g_cl[i].state == C_DRAIN) {
                        close_client(&g_cl[i]);
                        c = &g_cl[i];
                    }
                if (!c) {
                    Client *old = NULL;
                    for (int i = 0; i < SRV_CLIENTS; i++)
                        if (g_cl[i].state == C_READ && g_cl[i].reqLen == 0 && (!old || g_cl[i].since < old->since))
                            old = &g_cl[i];
                    if (old) {
                        app_log("srv: all %d connections busy: an idle one makes room", SRV_CLIENTS);
                        close_client(old);
                        c = old;
                    }
                }
                if (!c) { /* then a file answer that has not moved for a while (the phone asked again) */
                    Client *old = NULL;
                    const u64 at = osGetTime();
                    for (int i = 0; i < SRV_CLIENTS; i++)
                        if (g_cl[i].state == C_SEND && g_cl[i].file && at - g_cl[i].lastSend > STALE_MS &&
                            (!old || g_cl[i].lastSend < old->lastSend))
                            old = &g_cl[i];
                    if (old) {
                        old->err = -1001; /* made room */
                        close_client(old);
                        c = old;
                    }
                }
                if (!c) { /* busy: the page retries */
                    app_log("srv: all %d connections busy: 503", SRV_CLIENTS);
                    static const char busyMsg[] = "HTTP/1.1 503 Service Unavailable\r\nRetry-After: 1\r\n"
                                                  "Content-Length: 0\r\nConnection: close\r\n\r\n";
                    send(fd, busyMsg, sizeof(busyMsg) - 1, 0);
                    close(fd);
                    continue;
                }
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
                c->fd = fd;
                c->reqLen = 0;
                c->state = C_READ;
                c->since = osGetTime();
                continue;
            }
            Client *c = &g_cl[idx[k]];
            if (pf[k].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                c->err = -(int)pf[k].revents; /* negative: poll flags, not errno */
                close_client(c);
            }
            else if (c->state == C_READ || c->state == C_DRAIN)
                on_read(c);
            else if (c->state == C_SEND)
                on_write(c);
        }
    }
    for (int i = 0; i < SRV_CLIENTS; i++)
        if (g_cl[i].state != C_FREE)
            close_client(&g_cl[i]);
}

/* ------------------------------------------------------------------ API */

static bool load_page(void)
{
    FILE *f = fopen(PAGE_PATH, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    g_pageLen = (u32)ftell(f);
    fseek(f, 0, SEEK_SET);
    g_page = malloc(g_pageLen);
    const bool ok = g_page && fread(g_page, 1, g_pageLen, f) == g_pageLen;
    fclose(f);
    return ok;
}

ServerState server_start(const char *name, const char *lang, const char *version, bool new3ds)
{
    if (g_thread)
        return g_stats.state;
    snprintf(g_offer, sizeof(g_offer), "%.31s", name);
    memset(&g_stats, 0, sizeof(g_stats));
    LightLock_Init(&g_lock);
    u32 wifi = 0;
    if (R_SUCCEEDED(acInit())) {
        ACU_GetWifiStatus(&wifi);
        acExit();
    }
    if (!wifi) {
        app_log("srv: no Wi-Fi");
        g_stats.state = SRV_NO_WIFI;
        return g_stats.state;
    }
    snprintf(g_info, sizeof(g_info), "\"lang\":\"%s\",\"version\":\"%s\",\"console\":\"%s\"", lang, version,
             new3ds ? "New 3DS" : "Old 3DS");
    g_stats.state = SRV_FAILED;
    if (!load_page()) {
        app_log("srv: page missing from romfs");
        return g_stats.state;
    }
    g_socBuf = memalign(0x1000, SOC_SIZE);
    bool ok = g_socBuf != NULL;
    for (int i = 0; i < SRV_CLIENTS && ok; i++) {
        g_cl[i].state = C_FREE;
        ok = (g_cl[i].buf = memalign(0x40, CHUNK)) != NULL;
    }
    Result rc = 0;
    if (!ok || R_FAILED(rc = socInit(g_socBuf, SOC_SIZE))) {
        app_log("srv: socInit failed (memory %d, rc %08lX)", ok, (unsigned long)rc);
        server_stop();
        return SRV_FAILED;
    }
    g_srv = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(SRV_PORT);
    a.sin_addr.s_addr = gethostid();
    const int yes = 1;
    if (g_srv >= 0) /* the port of the last session may linger a moment */
        setsockopt(g_srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    bool bound = false;
    for (int i = 0; i < 10 && g_srv >= 0 && !(bound = bind(g_srv, (struct sockaddr *)&a, sizeof(a)) == 0); i++)
        svcSleepThread(300 * 1000 * 1000);
    if (g_srv < 0 || !bound || listen(g_srv, SRV_CLIENTS) < 0 || !a.sin_addr.s_addr) {
        app_log("srv: socket %d, bound %d, address %08lX, errno %d", g_srv, bound, (unsigned long)a.sin_addr.s_addr,
                errno);
        server_stop();
        g_stats.state = SRV_FAILED;
        return SRV_FAILED;
    }
    fcntl(g_srv, F_SETFL, fcntl(g_srv, F_GETFL, 0) | O_NONBLOCK);
    struct in_addr ip = {(in_addr_t)gethostid()};
    snprintf(g_stats.address, sizeof(g_stats.address), "http://%s:%d", inet_ntoa(ip), SRV_PORT);
    snprintf(g_stats.url, sizeof(g_stats.url), "%s/?f=%s", g_stats.address, g_offer);
    g_quit = false;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    /* above the UI: it mostly waits in poll(), and a UI frame must not hold up the next send
       (behind the UI it got ~80 KB/s, O3DS 2026-11-06) */
    g_thread = threadCreate(thread_main, NULL, 16 * 1024, prio - 1, -2, false);
    if (!g_thread) {
        server_stop();
        g_stats.state = SRV_FAILED;
        return SRV_FAILED;
    }
    g_stats.state = SRV_RUNNING;
    return SRV_RUNNING;
}

void server_stop(void)
{
    if (g_thread) {
        g_quit = true;
        threadJoin(g_thread, U64_MAX);
        threadFree(g_thread);
        g_thread = NULL;
    }
    if (g_srv >= 0)
        close(g_srv);
    g_srv = -1;
    if (g_socBuf) {
        socExit();
        free(g_socBuf);
        g_socBuf = NULL;
    }
    for (int i = 0; i < SRV_CLIENTS; i++) {
        free(g_cl[i].buf);
        g_cl[i].buf = NULL;
    }
    free(g_page);
    g_page = NULL;
    g_stats.state = SRV_OFF;
}

void server_stats(ServerStats *out)
{
    LightLock_Lock(&g_lock);
    *out = g_stats;
    const u64 slot = osGetTime() / 250;
    u64 sum = 0;
    for (u64 s = slot - 11; s <= slot; s++) /* the current slot is partly filled: 11.x slots */
        if (s > g_slotAt - 12 && s <= g_slotAt)
            sum += g_slots[s % 12];
    LightLock_Unlock(&g_lock);
    out->kbPerSec = (u32)(sum / 1024 / 3);
}
