#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* The module writes the same text when the file is missing (sysmod LoadConfig). */
static const char DEFAULTS[] = "; 3DS Recorder settings (read when the recorder starts)\n"
                               "; hotkey: buttons joined by +  (A B X Y L R START SELECT UP DOWN LEFT RIGHT)\n"
                               "hotkey=R+SELECT\n"
                               "; fps: 30, 20 or 15\n"
                               "fps=30\n"
                               "; screens: both, top or bottom\n"
                               "screens=both\n"
                               "; quality: auto or lossless\n"
                               "quality=auto\n"
                               "; led: pulse, solid or off\n"
                               "led=pulse\n"
                               "; audio: on or off\n"
                               "audio=on\n";

#define MAX_FILE 4096

static size_t ReadAll(char *buf, size_t cap)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return n;
}

void config_load(Config *c)
{
    c->fps = 30;
    c->screens = SCREENS_BOTH;
    c->lossless = false;
    c->audio = true;
    c->led = 2;
    snprintf(c->hotkey, sizeof(c->hotkey), "R+SELECT");
    c->ukrainian = false;
    c->guideSeen = false;
    c->langSet = false;
    c->replay = false;
    snprintf(c->replayHotkey, sizeof(c->replayHotkey), "X+SELECT");   /* the module's default too */
    static char buf[MAX_FILE];
    if (!ReadAll(buf, sizeof(buf)))
        return;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        while (*line == ' ' || *line == '\t')
            line++;
        if (*line == ';' || *line == '#')
            continue;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *v = eq + 1;
        v[strcspn(v, "\r ;#")] = 0;
        if (strcasecmp(line, "fps") == 0) {
            int fps = atoi(v);
            if (fps == 30 || fps == 20 || fps == 15)
                c->fps = fps;
        } else if (strcasecmp(line, "screens") == 0)
            c->screens = strcasecmp(v, "top") == 0      ? SCREENS_TOP
                         : strcasecmp(v, "bottom") == 0 ? SCREENS_BOTTOM
                                                        : SCREENS_BOTH;
        else if (strcasecmp(line, "quality") == 0)
            c->lossless = strcasecmp(v, "lossless") == 0;
        else if (strcasecmp(line, "audio") == 0)
            c->audio = strcasecmp(v, "off") != 0;
        else if (strcasecmp(line, "led") == 0)
            c->led = strcasecmp(v, "off") == 0 ? 0 : strcasecmp(v, "solid") == 0 ? 1 : 2;
        else if (strcasecmp(line, "hotkey") == 0 && *v)
            snprintf(c->hotkey, sizeof(c->hotkey), "%s", v);
        else if (strcasecmp(line, "lang") == 0) {
            c->ukrainian = strcasecmp(v, "uk") == 0;
            c->langSet = true;
        }
        else if (strcasecmp(line, "replay") == 0)
            c->replay = strcasecmp(v, "on") == 0;
        else if (strcasecmp(line, "replay_hotkey") == 0 && *v)
            snprintf(c->replayHotkey, sizeof(c->replayHotkey), "%s", v);
        else if (strcasecmp(line, "guide") == 0)
            c->guideSeen = strcasecmp(v, "done") == 0;
    }
}

bool config_set(const char *key, const char *value)
{
    static char in[MAX_FILE], out[MAX_FILE + 128];
    size_t n = ReadAll(in, sizeof(in));
    if (!n) {
        mkdir("sdmc:/3ds", 0777);
        mkdir("sdmc:/3ds/recorder", 0777);
        snprintf(in, sizeof(in), "%s", DEFAULTS);
        n = strlen(in);
    }
    size_t o = 0, klen = strlen(key);
    bool done = false;
    const char *p = in;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) + 1 : strlen(p);
        const char *q = p;
        while (*q == ' ' || *q == '\t')
            q++;
        const bool match = strncasecmp(q, key, klen) == 0 && (q[klen] == '=' || q[klen] == ' ');
        if (match && !done) {
            o += (size_t)snprintf(out + o, sizeof(out) - o, "%s=%s\n", key, value);
            done = true;
        } else if (!match && o + len < sizeof(out)) {
            memcpy(out + o, p, len);
            o += len;
        }
        p += len;
    }
    if (!done) {
        if (o && out[o - 1] != '\n' && o + 1 < sizeof(out))
            out[o++] = '\n';
        o += (size_t)snprintf(out + o, sizeof(out) - o, "%s=%s\n", key, value);
    }
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f)
        return false;
    const bool ok = fwrite(out, 1, o, f) == o;
    fclose(f);
    return ok;
}
