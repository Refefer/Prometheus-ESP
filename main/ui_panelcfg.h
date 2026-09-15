/*
 * Per-tile settings: widget type, size, and whether the tile shows one series
 * or every series of its metric.
 *
 * Reached by tapping a tile. Auto-inference gets the common case right, but
 * "right" is a judgement about what you want to look at, so it has to be
 * changeable without a rebuild.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

void ui_panelcfg_open(uint16_t panel_id, void (*on_close)(void));
bool ui_panelcfg_is_open(void);
