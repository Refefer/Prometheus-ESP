/*
 * The metric browser: scrape the endpoint, list everything it exposes, and
 * tick what should appear on the dashboard.
 *
 * Browsing is by metric NAME rather than by series. A node_exporter emits
 * ~1800 series but only ~300 distinct names, and names are what people think
 * in -- a five-fold smaller list that reads the way the metric is documented.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Browse-and-tick: add or remove any number of tiles, auto-placed on the
 * given screen. */
void ui_browser_open(uint8_t screen, void (*on_close)(void));

/*
 * Pick-one: choose a single metric for a specific empty cell.
 *
 * Reached by tapping the empty outline where the tile should go, which is a
 * more direct way to build a screen than picking from a list and finding out
 * afterwards where it landed. on_pick receives the new panel's id, or 0 if
 * the user backed out.
 */
void ui_browser_open_pick(uint8_t screen, uint8_t col, uint8_t row,
                          void (*on_pick)(uint16_t panel_id));

/*
 * Select-a-selector: returns a series' selector without creating anything.
 * Used to choose the second operand of a derived panel, so it lists the
 * endpoint of `screen` -- the one the panel already reads. NULL means
 * cancelled.
 */
void ui_browser_open_select(uint8_t screen, void (*on_select)(const char *sel));

bool ui_browser_is_open(void);
