/*
 * First-boot wizard: welcome -> WiFi -> (endpoint, next milestone).
 *
 * Credentials are only ever typed on the glass and go straight to NVS, so
 * they never exist in a config file, a build artefact, or a chat transcript.
 */
#pragma once

#include <stdbool.h>

/* Builds the wizard over whatever is on screen. Call under the LVGL lock. */
void ui_setup_open(void);
bool ui_setup_is_open(void);
