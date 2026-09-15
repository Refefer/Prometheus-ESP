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

void ui_browser_open(void (*on_close)(void));
bool ui_browser_is_open(void);
