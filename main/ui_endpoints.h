/* The endpoint editor: type a URL on the glass and test it before saving. */
#pragma once
#include <stdbool.h>

/* Opens over whatever is on screen. `on_close` fires after the overlay is
 * gone, so the caller can rebuild the dashboard. Call under the LVGL lock. */
void ui_endpoints_open(void (*on_close)(void));
bool ui_endpoints_is_open(void);
