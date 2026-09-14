/*
 * Display bring-up for the Waveshare ESP32-S3-Touch-LCD-7 (plain, 800x480 RGB,
 * GT911 touch, CH422G IO expander).
 *
 * Extracted from theqkash/esp32flight (MIT), which adapted it from Waveshare's
 * 08_lvgl_Porting demo (CC0-1.0). All multi-board support stripped: diff
 * against esp32flight's copy to see what supporting more panels costs.
 */
#pragma once

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "lvgl_port.h"

/* I2C bus shared by the CH422G expander, the GT911 touch controller and the
 * external I2C terminal. Addresses 0x20-0x27/0x30-0x3F (CH422G) and 0x5D/0x14
 * (GT911) are taken. */
#define I2C_MASTER_SCL_IO           9
#define I2C_MASTER_SDA_IO           8
#define I2C_MASTER_NUM              0
#define I2C_MASTER_FREQ_HZ          400000
#define I2C_MASTER_TIMEOUT_MS       1000

/* GT911 INT pin, driven low during reset to select I2C addr 0x5D */
#define GPIO_TOUCH_INT              4

#define EXAMPLE_LCD_H_RES               (LVGL_PORT_H_RES)
#define EXAMPLE_LCD_V_RES               (LVGL_PORT_V_RES)
/* Scanout bandwidth knob: 16MHz -> ~31Hz refresh with headroom (esp32flight's
 * shipping value); Waveshare's benchmark pushes 21MHz for ~41Hz. */
#define EXAMPLE_LCD_PIXEL_CLOCK_HZ      (16 * 1000 * 1000)
#define EXAMPLE_RGB_BIT_PER_PIXEL       (16)
#define EXAMPLE_RGB_DATA_WIDTH          (16)
/* Bounce buffers: small internal-SRAM staging chunks the LCD DMA reads from
 * while the CPU tops them up from the PSRAM framebuffer */
#define EXAMPLE_RGB_BOUNCE_BUFFER_HEIGHT (10)
#define EXAMPLE_RGB_BOUNCE_BUFFER_SIZE  (EXAMPLE_LCD_H_RES * EXAMPLE_RGB_BOUNCE_BUFFER_HEIGHT)

/* RGB565 wiring: DATA0..4 = B3..B7, DATA5..10 = G2..G7, DATA11..15 = R3..R7
 * (the panel's low color bits are unwired - 16 data pins is already most of
 * the chip's GPIO budget) */
#define EXAMPLE_LCD_IO_RGB_VSYNC        (GPIO_NUM_3)
#define EXAMPLE_LCD_IO_RGB_HSYNC        (GPIO_NUM_46)
#define EXAMPLE_LCD_IO_RGB_DE           (GPIO_NUM_5)
#define EXAMPLE_LCD_IO_RGB_PCLK         (GPIO_NUM_7)
#define EXAMPLE_LCD_IO_RGB_DATA0        (GPIO_NUM_14)
#define EXAMPLE_LCD_IO_RGB_DATA1        (GPIO_NUM_38)
#define EXAMPLE_LCD_IO_RGB_DATA2        (GPIO_NUM_18)
#define EXAMPLE_LCD_IO_RGB_DATA3        (GPIO_NUM_17)
#define EXAMPLE_LCD_IO_RGB_DATA4        (GPIO_NUM_10)
#define EXAMPLE_LCD_IO_RGB_DATA5        (GPIO_NUM_39)
#define EXAMPLE_LCD_IO_RGB_DATA6        (GPIO_NUM_0)
#define EXAMPLE_LCD_IO_RGB_DATA7        (GPIO_NUM_45)
#define EXAMPLE_LCD_IO_RGB_DATA8        (GPIO_NUM_48)
#define EXAMPLE_LCD_IO_RGB_DATA9        (GPIO_NUM_47)
#define EXAMPLE_LCD_IO_RGB_DATA10       (GPIO_NUM_21)
#define EXAMPLE_LCD_IO_RGB_DATA11       (GPIO_NUM_1)
#define EXAMPLE_LCD_IO_RGB_DATA12       (GPIO_NUM_2)
#define EXAMPLE_LCD_IO_RGB_DATA13       (GPIO_NUM_42)
#define EXAMPLE_LCD_IO_RGB_DATA14       (GPIO_NUM_41)
#define EXAMPLE_LCD_IO_RGB_DATA15       (GPIO_NUM_40)

/* Panel + touch + LVGL init, in dependency order (I2C -> CH422G -> RGB panel
 * -> GT911 -> LVGL). Backlight stays off until waveshare_rgb_lcd_bl_on(). */
esp_err_t waveshare_esp32_s3_rgb_lcd_init(void);

esp_err_t waveshare_rgb_lcd_bl_on(void);
esp_err_t waveshare_rgb_lcd_bl_off(void);
