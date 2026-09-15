/*
 * The layout picker: switch which dashboard is on the glass.
 *
 * Layouts exist because one endpoint can serve completely different metrics
 * depending on what is running behind it. Saving and switching them over HTTP
 * works, but needing a laptop to change what a wall panel shows defeats the
 * point of a touchscreen, so the same operations live here.
 */
#pragma once
#include <stdbool.h>

/* Opens over whatever is on screen. `on_close` fires after the overlay is
 * gone, so the caller can rebuild the dashboard. Call under the LVGL lock. */
void ui_layouts_open(void (*on_close)(void));
bool ui_layouts_is_open(void);
