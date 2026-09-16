/*
 * Wall-clock time, for the header readout.
 *
 * Kept apart from the monotonic clock everything else uses: rates, windows
 * and staleness are all measured with esp_timer, which starts at boot and
 * never jumps. This is the only thing that wants the actual time of day, and
 * it is the only thing that can be wrong for the first few seconds of a boot
 * or for as long as the network is down.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Begins SNTP. Safe to call before the link is up; it retries on its own. */
void timekeep_start(void);

/* A POSIX TZ string, e.g. "UTC0", "PST8PDT,M3.2.0,M11.1.0", "GMT0BST,M3.5.0/1,M10.5.0". */
void timekeep_set_tz(const char *tz);

/* False until the clock has actually been set, so the UI can show nothing
 * rather than 1970. */
bool timekeep_valid(void);

/* "14:32" and "Mon 16 Sep". Either pointer may be NULL. */
void timekeep_now(char *time_out, size_t tcap, char *date_out, size_t dcap);
