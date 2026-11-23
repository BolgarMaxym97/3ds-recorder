#include "support.h"

#include <3ds.h>
#include <string.h>

#include "net.h"
#include "recorder.h"
#include "stats.h"

#define TEXT_MAX 1024

typedef enum { SUP_NOT, SUP_RUNNING, SUP_READY, SUP_FAILED } SupState;

static Thread g_thread;
static NetConn g_net;
static volatile bool g_stop;
static volatile u32 g_result;
static volatile SupState g_state;
static volatile int g_count;
static SupportLink g_links[SUPPORT_MAX];
static char g_text[TEXT_MAX + 1];
static size_t g_len;

static bool ToText(const u8 *data, u32 len, void *ctx)
{
    (void)ctx;
    if (g_len + len > TEXT_MAX)
        return false;
    memcpy(g_text + g_len, data, len);
    g_len += len;
    return true;
}

static SupState Fetch(void)
{
    u32 wifi = 0;
    if (R_SUCCEEDED(acInit())) {
        ACU_GetWifiStatus(&wifi);
        acExit();
    }
    if (!wifi || g_stop)
        return SUP_FAILED;
    if (R_FAILED(httpcInit(NET_HTTPC_SHM)))
        return SUP_FAILED;
    u32 status = 0;
    g_len = 0;
    const bool ok = net_get(&g_net, "/v1/support", 0, ToText, NULL, &status, NULL, NULL, 0);
    if (!g_stop)   /* the app is closing and may not have waited for this thread */
        httpcExit();
    if (!ok) {
        app_log("support: failed (HTTP %lu, 0x%08lX)", (unsigned long)status, (unsigned long)g_result);
        return SUP_FAILED;
    }
    const int n = support_parse(g_text, g_len, g_links, SUPPORT_MAX);
    if (n)
        app_log("support: %d links", n);
    else
        app_log("support: none");
    g_count = n;
    return SUP_READY;
}

static void FetchThread(void *arg)
{
    (void)arg;
    g_state = Fetch();
}

static void Join(void)
{
    if (!g_thread)
        return;
    threadJoin(g_thread, U64_MAX);
    threadFree(g_thread);
    g_thread = NULL;
}

void support_fetch(void)
{
    if (g_state == SUP_RUNNING || g_state == SUP_READY)
        return;
    Join();
    static bool init;
    if (!init) {
        net_conn_init(&g_net, &g_stop, NULL, &g_result);
        init = true;
    }
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_state = SUP_RUNNING;
    g_thread = threadCreate(FetchThread, NULL, 16 * 1024, prio + 2 > 0x3F ? 0x3F : prio + 2, -2, false);
    if (!g_thread)
        g_state = SUP_FAILED;
}

int support_count(void) { return g_state == SUP_READY ? g_count : 0; }

const SupportLink *support_link(int i) { return i >= 0 && i < support_count() ? &g_links[i] : NULL; }

void support_stop(void)
{
    if (!g_thread)
        return;
    g_stop = true;
    net_abort(&g_net);
    Join();
}

#ifdef UI_TOUR
void support_tour(void)
{
    static const char text[] = "Ko-fi https://ko-fi.com/maksymbolhar\nMonobank https://send.monobank.ua/jar/9jJDGho7H8\n";
    g_count = support_parse(text, sizeof(text) - 1, g_links, SUPPORT_MAX);
    g_state = SUP_READY;
}
#endif
