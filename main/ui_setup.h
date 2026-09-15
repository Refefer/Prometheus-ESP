/*
 * First-boot wizard: welcome -> WiFi -> (endpoint, next milestone).
 *
 * Credentials are only ever typed on the glass and go straight to NVS, so
 * they never exist in a config file, a build artefact, or a chat transcript.
 */
#pragma once

#include <stdbool.h>

/*
 * Builds the wizard over whatever is on screen. Call under the LVGL lock.
 *
 * `on_close` fires after the overlay is gone so the caller can rebuild. It is
 * always possible to leave, including on first boot with no credentials: a
 * screen with no way out is worse than a dashboard that cannot fill itself,
 * and the WiFi button is one tap away.
 */
void ui_setup_open(void (*on_close)(void));
void ui_setup_close(void);
bool ui_setup_is_open(void);
