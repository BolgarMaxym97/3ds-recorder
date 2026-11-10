/*
 * sdmc:/3ds/recorder/config.ini, shared with the recorder module (which reads it when a
 * recording starts and ignores keys it does not know, such as lang). The app changes single
 * keys and keeps every other line, comments included.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#define CONFIG_PATH "sdmc:/3ds/recorder/config.ini"

enum { SCREENS_BOTH, SCREENS_TOP, SCREENS_BOTTOM };

typedef struct {
    int fps;          /* 30, 20, 15 */
    int screens;      /* SCREENS_* */
    bool audio;
    int led;          /* 0 off, 1 solid, 2 pulse */
    char hotkey[48];  /* as written, e.g. R+SELECT */
    bool ukrainian;   /* lang=uk; English otherwise (the default) */
    bool langSet;     /* config.ini has lang=: the language was chosen (first run asks) */
    bool guideSeen;   /* guide=done: the first-run guide was shown */
} Config;

void config_load(Config *c);
/* Sets key=value: replaces the key's line, or appends one. Creates the file with the module's
 * defaults when it is missing. */
bool config_set(const char *key, const char *value);

#endif
