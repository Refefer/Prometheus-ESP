#include "timekeep.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "time";
static bool s_started;

/*
 * Logged on every sync, because the failure mode is silent: the clock keeps
 * advancing on the RTC's internal RC oscillator whether or not SNTP ever
 * reaches anything, and that oscillator is uncalibrated -- it will drift
 * minutes per hour while looking perfectly plausible.
 */
static void on_sync(struct timeval *tv)
{
    struct tm tm;
    localtime_r(&tv->tv_sec, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    ESP_LOGI(TAG, "clock synced: %s", buf);
}

/*
 * 2024-01-01. Anything earlier is the epoch showing through rather than a
 * time anyone set, which is what distinguishes "not synced yet" from "synced
 * to something odd".
 */
#define PLAUSIBLE_AFTER 1704067200

void timekeep_set_tz(const char *tz)
{
    setenv("TZ", (tz && tz[0]) ? tz : "UTC0", 1);
    tzset();
}

void timekeep_start(void)
{
    if (s_started) return;
    s_started = true;

    /*
     * Pool round-robin, and the default SMOOTH-off mode: a wall panel wants
     * the clock correct as soon as it can be, and there is nothing running
     * here that a step backwards would upset -- every measurement in this
     * app is taken against esp_timer, not against the wall clock.
     */
    /*
     * Plain and immediate. Two of the tempting options here do not do what
     * they read like: server_from_dhcp defers to a DHCP offer that only
     * arrives on a lease event, and renew_servers_after_new_IP waits for a
     * GOT_IP that has already fired by the time anything calls this. Either
     * one leaves the client never started -- enabled=0, server unresolved,
     * and a clock that quietly free-runs on the RTC instead.
     */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.start   = true;
    cfg.sync_cb = on_sync;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sntp did not start: %s", esp_err_to_name(err));
        s_started = false;
        return;
    }
    ESP_LOGI(TAG, "sntp started");
}

bool timekeep_valid(void)
{
    return time(NULL) > PLAUSIBLE_AFTER;
}

void timekeep_now(char *time_out, size_t tcap, char *date_out, size_t dcap)
{
    if (time_out && tcap) time_out[0] = '\0';
    if (date_out && dcap) date_out[0] = '\0';
    if (!timekeep_valid()) return;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (time_out && tcap) strftime(time_out, tcap, "%H:%M", &tm);
    if (date_out && dcap) strftime(date_out, dcap, "%a %-d %b", &tm);
}
