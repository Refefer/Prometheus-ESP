/*
 * Flash-backed storage: NVS for secrets, LittleFS for configuration.
 *
 * The split is deliberate. WiFi credentials and per-endpoint auth tokens live
 * in NVS (the WiFi driver needs an NVS partition anyway, and a corrupt config
 * file then still leaves the device on the network and recoverable). Anything
 * non-secret lives in a JSON file on LittleFS, which means it can be dumped
 * over serial, pasted into a bug report, and grown past what an NVS blob can
 * comfortably hold.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#define STORAGE_CFG_PATH  "/cfg"
#define STORAGE_DATA_PATH "/data"

/* NVS init (erasing and retrying if the partition is from an older layout),
 * then both LittleFS mounts. Individual mount failures are logged and
 * reported through the accessors below rather than aborting the boot -- a
 * panel that comes up read-only and says so is far more useful than one that
 * sits in a boot loop on a wall. */
esp_err_t storage_init(void);

bool storage_cfg_ready(void);
bool storage_data_ready(void);

/* Bytes total/used on a mounted partition; zeroed when it is not mounted. */
void storage_usage(const char *base_path, size_t *total, size_t *used);
