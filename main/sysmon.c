#include "sysmon.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "sysmon";

#define SYSMON_PERIOD_US (5 * 1000 * 1000)

/*
 * The run-time counter is esp_timer microseconds truncated to 32 bits, so it
 * wraps every ~71 minutes. Every delta here is taken in uint32_t, which stays
 * correct across a wrap as long as one period is far shorter than that.
 */
static uint32_t s_prev_idle[portNUM_PROCESSORS];
static uint32_t s_prev_wall;
static volatile int s_busy[portNUM_PROCESSORS] = { -1, -1 };

static void sample(void *arg)
{
    (void)arg;
    uint32_t wall = (uint32_t)esp_timer_get_time();
    uint32_t dwall = wall - s_prev_wall;
    s_prev_wall = wall;

    for (int c = 0; c < portNUM_PROCESSORS; c++) {
        uint32_t idle = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(c);
        uint32_t didle = idle - s_prev_idle[c];
        s_prev_idle[c] = idle;
        if (dwall == 0) continue;
        int busy = 100 - (int)((uint64_t)didle * 100 / dwall);
        s_busy[c] = busy < 0 ? 0 : busy > 100 ? 100 : busy;
    }

    ESP_LOGI(TAG, "cpu0 %d%%  cpu1 %d%%  SRAM %uK (min %uK)  PSRAM %uK",
             s_busy[0], s_busy[1],
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

void sysmon_start(void)
{
    static esp_timer_handle_t t;
    if (t) return;

    s_prev_wall = (uint32_t)esp_timer_get_time();
    for (int c = 0; c < portNUM_PROCESSORS; c++) {
        s_prev_idle[c] = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(c);
    }

    const esp_timer_create_args_t args = {
        .callback = sample,
        .name = "sysmon",
    };
    if (esp_timer_create(&args, &t) != ESP_OK ||
        esp_timer_start_periodic(t, SYSMON_PERIOD_US) != ESP_OK) {
        ESP_LOGW(TAG, "load readout unavailable");
    }
}

int sysmon_cpu_pct(int core)
{
    if (core < 0 || core >= portNUM_PROCESSORS) return -1;
    return s_busy[core];
}

uint32_t sysmon_sram_min(void)
{
    return (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
}
