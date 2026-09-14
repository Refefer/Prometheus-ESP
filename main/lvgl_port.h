/*
 * LVGL v8 glue for the RGB panel: draw buffers, flush callback, tick source,
 * touch input device, and the task that owns the widget tree.
 *
 * Extracted from theqkash/esp32flight (MIT) / Espressif example code
 * (Apache-2.0), stripped of rotation and multi-panel support.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_types.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LVGL_PORT_H_RES             (800)
#define LVGL_PORT_V_RES             (480)
#define LVGL_PORT_TICK_PERIOD_MS    (2)

#define LVGL_PORT_TASK_MAX_DELAY_MS (500)
#define LVGL_PORT_TASK_MIN_DELAY_MS (10)
#define LVGL_PORT_TASK_STACK_SIZE   (6 * 1024)
#define LVGL_PORT_TASK_PRIORITY     (2)
/* Core 1: keep rendering away from core 0, where WiFi/BT and most system
 * tasks run by default */
#define LVGL_PORT_TASK_CORE         (1)

/*
 * The buffer-strategy knob. Two arrangements, one #define apart:
 *
 *  1 (tear-avoid, the esp32flight shipping config): two full 800x480
 *    framebuffers in PSRAM, LVGL "direct mode" renders dirty regions straight
 *    into the off-screen one, and the flush flips scanout on vsync. Costs an
 *    extra 750KB of PSRAM; no tearing.
 *
 *  0 (partial buffer): one framebuffer, plus a small LVGL draw buffer in
 *    internal SRAM; the flush memcpys each rendered strip into the live
 *    framebuffer. Cheap on memory, fast to render, but scanout can catch the
 *    copy mid-frame (tearing on large updates).
 */
#define LVGL_PORT_AVOID_TEAR_ENABLE (1)

#if LVGL_PORT_AVOID_TEAR_ENABLE
#define LVGL_PORT_LCD_RGB_BUFFER_NUMS   (2)
#define LVGL_PORT_DIRECT_MODE           (1)
#else
#define LVGL_PORT_LCD_RGB_BUFFER_NUMS   (1)
#define LVGL_PORT_DIRECT_MODE           (0)
/* Draw-buffer strip height in rows; 100 rows x 800 px x 2B = ~156KB of SRAM */
#define LVGL_PORT_BUFFER_HEIGHT         (100)
#define LVGL_PORT_BUFFER_MALLOC_CAPS    (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif

esp_err_t lvgl_port_init(esp_lcd_panel_handle_t lcd_handle, esp_lcd_touch_handle_t tp_handle);

/* LVGL is single-threaded: any call that touches widgets from outside the
 * LVGL task must be wrapped in lock/unlock. timeout_ms < 0 blocks forever. */
bool lvgl_port_lock(int timeout_ms);
void lvgl_port_unlock(void);

/* Called from the LCD frame-done ISR; wakes the flush waiting on vsync. */
bool lvgl_port_notify_rgb_vsync(void);

#ifdef __cplusplus
}
#endif
