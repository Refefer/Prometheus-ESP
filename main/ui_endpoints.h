/*
 * The settings sheet behind the gear: an Endpoints tab to add, edit, test and
 * remove endpoints, and a Device tab for theme, auto-rotate, Wi-Fi and the
 * config push token.
 */
#pragma once
#include <stdbool.h>

/* Opens over whatever is on screen. `on_close` fires after the overlay is
 * gone, so the caller can rebuild the dashboard. Call under the LVGL lock. */
void ui_endpoints_open(void (*on_close)(void));
bool ui_endpoints_is_open(void);
