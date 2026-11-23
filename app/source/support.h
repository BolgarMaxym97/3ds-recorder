/*
 * Help → "Support the project": the Ko-fi / Monobank links, asked from the Worker
 * (STATS_URL/v1/support, which reads .github/FUNDING.yml on GitHub) when Help opens, so a
 * changed link needs no app release. No Wi-Fi, no answer or no links: no topic. The request
 * carries nothing about the console.
 */
#ifndef SUPPORT_H
#define SUPPORT_H

#include "support_core.h"

/* Starts the fetch in the background unless the links are here or one is under way. */
void support_fetch(void);
/* The links (0 until they arrive). */
int support_count(void);
const SupportLink *support_link(int i);
/* At exit: ends the fetch under way. */
void support_stop(void);

#ifdef UI_TOUR
void support_tour(void);
#endif

#endif
