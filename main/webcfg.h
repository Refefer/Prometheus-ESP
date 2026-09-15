/*
 * A very small HTTP endpoint for pushing configuration.
 *
 * The touch UI is good for adjusting a tile; it is a poor place to express
 * sum over a glob-matched label set combined with another such set. This
 * serves the same JSON that lives on flash, so the workflow is fetch, edit in
 * a real editor, push back -- and the config is version-controllable.
 *
 *   GET  /config   -> the current configuration
 *   POST /config   <- replace it; applies immediately and persists
 *   GET  /status   -> device identity and health, no auth required
 *
 * Both /config methods require the push token in an X-Auth header. It is
 * generated on first use, lives in NVS, and is shown on the device.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t webcfg_start(void);
void      webcfg_stop(void);
bool      webcfg_running(void);

/* True once a request has authenticated successfully. Until then the device
 * repeats the curl line on the console, because a token you cannot read is a
 * device you cannot configure. */
bool      webcfg_ever_used(void);
