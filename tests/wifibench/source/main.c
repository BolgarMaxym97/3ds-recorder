/*
 * wifibench — measures how fast a phone / PC can download from the 3DS over Wi-Fi
 * (roadmap phase 4: transfer of recordings, research 2026-11-03).
 *
 * A minimal HTTP/1.0 server on port 8080, up to MAX_CLIENTS downloads at once
 * (non-blocking sockets + poll, one thread):
 *   GET /zero?mb=N&kb=K          N MB generated in memory, K KB per send() (network only)
 *   GET /file?off=O&mb=N&kb=K    N MB (0 = to the end) from offset O MB of the newest recording in
 *                                sdmc:/3ds/recorder/recordings (SD read + network, the real case)
 *   GET /                        a small page with links (open it on a phone)
 *   any: &sndbuf=K sets SO_SNDBUF to K KB for that connection
 * Several parallel requests show whether more connections give more throughput
 * (tools/wifibench.sh <ip> runs 1-4 at once). The top screen lists every finished
 * transfer; sdmc:/3ds/wifibench.log keeps them. START exits.
 */
#include <3ds.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT        8080
#define SOC_SIZE    0x100000u          /* SOC service buffer (must be 0x1000 aligned) */
#define MAX_CHUNK   (256u * 1024u)
#define FILE_CHUNK  (64u * 1024u)
#define MAX_CLIENTS 4
#define REC_DIR     "sdmc:/3ds/recorder/recordings"

typedef enum { C_FREE, C_READ, C_SEND } CState;
typedef struct {
    CState state;
    int fd;
    char req[512];
    int reqLen;
    char head[320];
    int headLen, headPos;
    FILE *f;               /* NULL: generated data */
    u8 *buf;               /* FILE_CHUNK, for file reads */
    size_t bufLen, bufPos;
    size_t chunk;          /* bytes per send() */
    unsigned long long left, sent;
    double t0, sdSecs;
    char what[40];
    const u8 *body;        /* small static body (index page) */
} Client;

static u8 *g_pattern;
static Client g_cl[MAX_CLIENTS];
static PrintConsole g_top, g_bot;

static const char PAGE[] =
    "<!doctype html><meta name=viewport content='width=device-width'><title>wifibench</title>"
    "<body style='font:18px system-ui;padding:16px'><h2>3DS wifibench</h2>"
    "<p><a href='/zero?mb=16&kb=16'>16 MB from memory</a></p>"
    "<p><a href='/file?mb=32'>32 MB of the newest recording</a></p>"
    "<p><a href='/file'>whole newest recording</a></p></body>";

static double Now(void) { return osGetTime() / 1000.0; }

static long Param(const char *path, const char *key, long def)
{
    const size_t k = strlen(key);
    for (const char *p = strchr(path, '?'); p && *p; p = strchr(p + 1, '&'))
        if (strncmp(p + 1, key, k) == 0 && p[1 + k] == '=')
            return strtol(p + 2 + k, NULL, 10);
    return def;
}

/* newest .r3d by name (names are yyyymmdd_hhmmss) */
static bool NewestRecording(char *out, size_t cap)
{
    DIR *d = opendir(REC_DIR);
    if (!d)
        return false;
    char best[64] = "";
    struct dirent *e;
    while ((e = readdir(d)))
    {
        const size_t n = strlen(e->d_name);
        if (n > 4 && n < sizeof(best) && strcmp(e->d_name + n - 4, ".r3d") == 0 && strcmp(e->d_name, best) > 0)
            strcpy(best, e->d_name);
    }
    closedir(d);
    if (!best[0])
        return false;
    snprintf(out, cap, REC_DIR "/%s", best);
    return true;
}

static int Active(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        n += g_cl[i].state == C_SEND;
    return n;
}

static void Report(const Client *c, bool ok)
{
    const double secs = Now() - c->t0, kbs = secs > 0 ? c->sent / 1024.0 / secs : 0;
    consoleSelect(&g_top);
    printf("%-24s %6.1f MB %6.1f s %5.0f KB/s%s\n", c->what, c->sent / 1048576.0, secs, kbs, ok ? "" : " cut");
    if (c->f)
        printf("  (SD reads %.1f s)\n", c->sdSecs);
    FILE *log = fopen("sdmc:/3ds/wifibench.log", "a");
    if (log)
    {
        fprintf(log, "%s %.1f MB %.2f s %.0f KB/s%s sd %.2f s\n", c->what, c->sent / 1048576.0, secs, kbs,
                ok ? "" : " (cut)", c->sdSecs);
        fclose(log);
    }
}

static void Close(Client *c, bool report, bool ok)
{
    if (report)
        Report(c, ok);
    if (c->f)
        fclose(c->f);
    c->f = NULL;
    shutdown(c->fd, SHUT_RDWR);
    close(c->fd);
    c->state = C_FREE;
}

static void SetHead(Client *c, const char *status, const char *type, unsigned long long len, const char *name)
{
    int n = snprintf(c->head, sizeof(c->head), "HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %llu\r\nConnection: close\r\n",
                     status, type, len);
    if (name)
        n += snprintf(c->head + n, sizeof(c->head) - n, "Content-Disposition: attachment; filename=\"%s\"\r\n", name);
    n += snprintf(c->head + n, sizeof(c->head) - n, "\r\n");
    c->headLen = n;
    c->headPos = 0;
}

/* request complete: decide what to send */
static void Start(Client *c)
{
    char path[256] = "/";
    sscanf(c->req, "GET %255s", path);
    consoleSelect(&g_bot);
    printf("[%d] GET %s\n", Active() + 1, path);
    const long sndbuf = Param(path, "sndbuf", 0);
    if (sndbuf > 0)
    {
        const int v = (int)(sndbuf * 1024);
        setsockopt(c->fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v));
    }
    long kb = Param(path, "kb", 16);
    kb = kb < 1 ? 1 : kb > (long)(MAX_CHUNK / 1024) ? (long)(MAX_CHUNK / 1024) : kb;
    c->chunk = (size_t)kb * 1024;
    c->f = NULL;
    c->body = NULL;
    c->sent = c->sdSecs = 0;
    c->bufLen = c->bufPos = 0;
    if (strncmp(path, "/zero", 5) == 0)
    {
        c->left = (unsigned long long)Param(path, "mb", 16) * 1048576ull;
        SetHead(c, "200 OK", "application/octet-stream", c->left, "zero.bin");
        snprintf(c->what, sizeof(c->what), "mem %ldK", kb);
    }
    else if (strncmp(path, "/file", 5) == 0)
    {
        char file[128];
        c->f = NewestRecording(file, sizeof(file)) ? fopen(file, "rb") : NULL;
        if (!c->f)
        {
            SetHead(c, "404 Not Found", "text/plain", 0, NULL);
            c->left = 0;
            snprintf(c->what, sizeof(c->what), "404");
        }
        else
        {
            setvbuf(c->f, NULL, _IONBF, 0);
            fseek(c->f, 0, SEEK_END);
            const unsigned long long size = (unsigned long long)ftell(c->f);
            unsigned long long off = (unsigned long long)Param(path, "off", 0) * 1048576ull;
            off = off > size ? size : off;
            const long mb = Param(path, "mb", 0);
            c->left = size - off;
            if (mb > 0 && (unsigned long long)mb * 1048576ull < c->left)
                c->left = (unsigned long long)mb * 1048576ull;
            fseek(c->f, (long)off, SEEK_SET);
            SetHead(c, "200 OK", "application/octet-stream", c->left, strrchr(file, '/') + 1);
            snprintf(c->what, sizeof(c->what), "SD @%lluM %ldK", off / 1048576ull, kb);
        }
    }
    else
    {
        c->body = (const u8 *)PAGE;
        c->left = sizeof(PAGE) - 1;
        SetHead(c, "200 OK", "text/html; charset=utf-8", c->left, NULL);
        snprintf(c->what, sizeof(c->what), "page");
    }
    c->state = C_SEND;
    c->t0 = Now();
}

/* socket readable */
static void OnRead(Client *c)
{
    const ssize_t r = recv(c->fd, c->req + c->reqLen, sizeof(c->req) - 1 - c->reqLen, 0);
    if (r <= 0)
    {
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        Close(c, false, false);
        return;
    }
    c->reqLen += (int)r;
    c->req[c->reqLen] = 0;
    if (strstr(c->req, "\r\n\r\n") || c->reqLen >= (int)sizeof(c->req) - 1)
        Start(c);
}

/* socket writable: header, then the body in c->chunk pieces */
static void OnWrite(Client *c)
{
    if (c->headPos < c->headLen)
    {
        const ssize_t w = send(c->fd, c->head + c->headPos, c->headLen - c->headPos, 0);
        if (w < 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                Close(c, true, false);
            return;
        }
        c->headPos += (int)w;
        return;
    }
    if (!c->left)
    {
        Close(c, c->body == NULL, true);
        return;
    }
    const u8 *p;
    size_t n;
    if (c->body)
    {
        p = c->body + c->sent;
        n = (size_t)c->left;
    }
    else if (c->f)
    {
        if (c->bufPos == c->bufLen)
        {
            const size_t want = c->left < FILE_CHUNK ? (size_t)c->left : FILE_CHUNK;
            const double r0 = Now();
            c->bufLen = fread(c->buf, 1, want, c->f);
            c->sdSecs += Now() - r0;
            c->bufPos = 0;
            if (!c->bufLen)
            {
                Close(c, true, false);
                return;
            }
        }
        p = c->buf + c->bufPos;
        n = c->bufLen - c->bufPos;
    }
    else
    {
        p = g_pattern + (c->sent % c->chunk);
        n = c->chunk - (size_t)(c->sent % c->chunk);
        if (n > c->left)
            n = (size_t)c->left;
    }
    if (n > c->chunk)
        n = c->chunk;
    const ssize_t w = send(c->fd, p, n, 0);
    if (w < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            Close(c, true, false);
        return;
    }
    c->sent += (unsigned long long)w;
    c->left -= (unsigned long long)w;
    if (c->f)
        c->bufPos += (size_t)w;
}

static void WaitStart(void)
{
    while (aptMainLoop())
    {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        gspWaitForVBlank();
    }
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, &g_top);
    consoleInit(GFX_BOTTOM, &g_bot);
    osSetSpeedupEnable(true);   /* New 3DS: 804 MHz */
    consoleSelect(&g_top);
    printf("wifibench v2: up to %d parallel (START exits)\n\n", MAX_CLIENTS);

    g_pattern = memalign(0x1000, MAX_CHUNK);
    u32 *socBuf = memalign(0x1000, SOC_SIZE);
    bool ok = g_pattern && socBuf;
    for (int i = 0; i < MAX_CLIENTS && ok; i++)
        ok = (g_cl[i].buf = memalign(0x40, FILE_CHUNK)) != NULL;
    if (!ok || R_FAILED(socInit(socBuf, SOC_SIZE)))
    {
        printf("socInit failed: is Wi-Fi on?\n");
        WaitStart();
        gfxExit();
        return 0;
    }
    for (u32 i = 0; i < MAX_CHUNK; i++)
        g_pattern[i] = (u8)(i * 31u + 7u);   /* not zeros: nothing on the way can compress it */

    const int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(PORT);
    a.sin_addr.s_addr = gethostid();
    if (srv < 0 || bind(srv, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(srv, MAX_CLIENTS) < 0)
    {
        printf("server failed: errno %d\n", errno);
        WaitStart();
        socExit();
        gfxExit();
        return 0;
    }
    fcntl(srv, F_SETFL, fcntl(srv, F_GETFL, 0) | O_NONBLOCK);
    struct in_addr ip = { (in_addr_t)gethostid() };
    printf("Open on the Mac / phone:\n  http://%s:%d/\n\n", inet_ntoa(ip), PORT);

    while (aptMainLoop())
    {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        struct pollfd pf[MAX_CLIENTS + 1];
        int idx[MAX_CLIENTS + 1], n = 0;
        pf[n].fd = srv;
        pf[n].events = POLLIN;
        idx[n++] = -1;
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (g_cl[i].state != C_FREE)
            {
                pf[n].fd = g_cl[i].fd;
                pf[n].events = g_cl[i].state == C_READ ? POLLIN : POLLOUT;
                idx[n++] = i;
            }
        if (poll(pf, n, 10) <= 0)
            continue;
        for (int k = 0; k < n; k++)
        {
            if (!pf[k].revents)
                continue;
            if (idx[k] < 0)
            {
                const int fd = accept(srv, NULL, NULL);
                if (fd < 0)
                    continue;
                Client *c = NULL;
                for (int i = 0; i < MAX_CLIENTS && !c; i++)
                    if (g_cl[i].state == C_FREE)
                        c = &g_cl[i];
                if (!c)
                {
                    close(fd);
                    continue;
                }
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
                c->fd = fd;
                c->reqLen = 0;
                c->state = C_READ;
                continue;
            }
            Client *c = &g_cl[idx[k]];
            if (pf[k].revents & (POLLERR | POLLHUP))
                Close(c, c->state == C_SEND, false);
            else if (c->state == C_READ)
                OnRead(c);
            else if (c->state == C_SEND)
                OnWrite(c);
        }
    }
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_cl[i].state != C_FREE)
            Close(&g_cl[i], false, false);
    close(srv);
    socExit();
    gfxExit();
    return 0;
}
