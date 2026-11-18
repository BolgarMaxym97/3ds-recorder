/*
 * Work with the lid closed (issue #5): while the background work (prepare.c: clips, joins, MP4s)
 * has something to do and this app is in front, the console is not let go to sleep when the lid
 * closes; the screens go dark and the work goes on. Once it is done, sleep is allowed again and
 * libctru (aptSetSleepAllowed -> APT_SleepIfShellClosed) puts the console to sleep at once.
 */
#ifndef LID_H
#define LID_H

#include <stdbool.h>

void lid_init(void);
void lid_exit(void);
/* Each main-loop pass; enabled: the setting (config.ini lid_work). True while the lid is closed:
   nothing is drawn then (the GPU waits for the lid otherwise and the loop would stop). */
bool lid_step(bool enabled);

#endif
