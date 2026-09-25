/*
 * Load readout: per-core CPU use and heap headroom, sampled every few seconds.
 *
 * Measured rather than estimated, because "do we have the compute for this"
 * is a question the device can answer about itself. Busy is 100% minus the
 * share of wall time each core's idle task ran, from FreeRTOS run-time stats.
 *
 * The same sample is logged as the serial heartbeat: the native-USB console
 * loses everything printed before a host attaches, so a periodic line is the
 * only way to see this device's state over serial.
 */
#pragma once

#include <stdint.h>

/* Starts the sampling timer. Safe to call once, from anywhere. */
void sysmon_start(void);

/* Busy percentage of a core over the last sample period, 0..100, or -1
 * before the first period has elapsed. */
int sysmon_cpu_pct(int core);

/* Lowest free internal SRAM since boot, in bytes. */
uint32_t sysmon_sram_min(void);
