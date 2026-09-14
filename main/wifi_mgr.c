#include "wifi_mgr.h"
#include "secrets.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "wifi";

#define BIT_CONNECTED BIT0
#define BIT_FAILED    BIT1

static EventGroupHandle_t s_events;
static esp_netif_t       *s_netif;
static esp_timer_handle_t s_retry_timer;
static volatile wifi_state_t s_state = WIFI_ST_IDLE;
static volatile bool      s_scanning;      /* pause reconnects during a scan */
static volatile bool      s_want_connect;  /* credentials exist; keep trying */
static char               s_fail[48] = "";
static uint8_t            s_retries;

/* Map the driver's numeric disconnect reason onto something a person setting
 * up a panel can act on. "error 205" helps nobody. */
static const char *reason_text(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "wrong password";
    case WIFI_REASON_NO_AP_FOUND:
        return "network not found";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_EXPIRE:
        return "access point refused";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "signal lost";
    case WIFI_REASON_AUTH_EXPIRE:
        return "authentication expired";
    default:
        return "connection failed";
    }
}

static void retry_cb(void *arg)
{
    (void)arg;
    if (!s_want_connect || s_scanning) return;
    ESP_LOGI(TAG, "reconnecting");
    esp_wifi_connect();
}

static void schedule_retry(uint32_t delay_ms)
{
    if (s_retry_timer == NULL) return;
    esp_timer_stop(s_retry_timer);
    /* One-shot esp_timer rather than a delay inside the event handler: the
     * event task must never block, or the whole WiFi stack stalls behind us. */
    esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;

    if (id == WIFI_EVENT_STA_START) {
        if (s_want_connect) { s_state = WIFI_ST_CONNECTING; esp_wifi_connect(); }
        return;
    }

    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        xEventGroupClearBits(s_events, BIT_CONNECTED);

        if (!s_want_connect || s_scanning) return;

        strncpy(s_fail, reason_text(d->reason), sizeof(s_fail) - 1);
        s_fail[sizeof(s_fail) - 1] = '\0';

        /*
         * A wrong password is not worth retrying at full speed -- it will
         * never succeed, and hammering the AP can get the device temporarily
         * banned. Everything else is usually transient (AP rebooting, signal
         * dropout) and deserves a quick retry.
         */
        bool fatal = (d->reason == WIFI_REASON_AUTH_FAIL ||
                      d->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT);
        if (fatal && s_retries >= 2) {
            s_state = WIFI_ST_FAILED;
            xEventGroupSetBits(s_events, BIT_FAILED);
            ESP_LOGW(TAG, "giving up: %s (reason %d)", s_fail, d->reason);
            return;
        }

        if (s_retries < 255) s_retries++;
        s_state = WIFI_ST_CONNECTING;
        /* Back off gently: 1s, 2s, 4s ... capped at 30s. A panel on a wall
         * should keep trying forever, just not busily. */
        uint32_t delay = 1000u << (s_retries < 5 ? s_retries : 5);
        if (delay > 30000) delay = 30000;
        ESP_LOGW(TAG, "disconnected: %s (reason %d), retry in %ums",
                 s_fail, d->reason, (unsigned)delay);
        schedule_retry(delay);
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;

    const ip_event_got_ip_t *e = data;
    ESP_LOGI(TAG, "connected, ip " IPSTR, IP2STR(&e->ip_info.ip));
    s_retries = 0;
    s_fail[0] = '\0';
    s_state   = WIFI_ST_CONNECTED;
    xEventGroupClearBits(s_events, BIT_FAILED);
    xEventGroupSetBits(s_events, BIT_CONNECTED);
}

esp_err_t wifi_mgr_start(void)
{
    s_events = xEventGroupCreate();
    if (s_events == NULL) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL));

    const esp_timer_create_args_t targs = {
        .callback = retry_cb, .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    char ssid[SECRETS_SSID_MAX] = "", pass[SECRETS_PASS_MAX] = "";
    secrets_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass));

    if (ssid[0] != '\0') {
        wifi_config_t wc = {0};
        strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
        strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        s_want_connect = true;
        s_state = WIFI_ST_CONNECTING;
    } else {
        /* No credentials yet. Start the radio anyway so the setup wizard can
         * scan -- this is the very first thing a new device needs to do. */
        s_want_connect = false;
        s_state = WIFI_ST_IDLE;
        ESP_LOGI(TAG, "no credentials stored; radio up for scanning only");
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

wifi_state_t wifi_mgr_state(void) { return s_state; }

bool wifi_mgr_is_connected(void)
{
    if (s_events == NULL) return false;
    return (xEventGroupGetBits(s_events) & BIT_CONNECTED) != 0;
}

const char *wifi_mgr_fail_reason(void) { return s_fail; }

esp_err_t wifi_mgr_connect(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    if (pass) strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);

    s_retries = 0;
    s_fail[0] = '\0';
    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_FAILED);

    esp_timer_stop(s_retry_timer);
    s_want_connect = false;         /* suppress the disconnect-driven retry */
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) return err;

    s_want_connect = true;
    s_state = WIFI_ST_CONNECTING;
    return esp_wifi_connect();
}

bool wifi_mgr_wait_connected(uint32_t timeout_ms)
{
    if (s_events == NULL) return false;
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED | BIT_FAILED,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & BIT_CONNECTED) != 0;
}

static int cmp_rssi(const void *a, const void *b)
{
    const wifi_ap_record_t *x = a, *y = b;
    return (y->rssi > x->rssi) - (y->rssi < x->rssi);   /* descending */
}

esp_err_t wifi_mgr_scan(wifi_ap_record_t *out, uint16_t *n_inout)
{
    if (out == NULL || n_inout == NULL || *n_inout == 0) return ESP_ERR_INVALID_ARG;

    s_scanning = true;
    esp_timer_stop(s_retry_timer);

    /* Scanning while associated works but is slow and lossy; drop the link
     * for the duration and let the retry loop restore it afterwards. */
    esp_wifi_disconnect();

    esp_err_t err = esp_wifi_scan_start(NULL, true);   /* blocking */
    if (err == ESP_OK) {
        uint16_t found = *n_inout;
        err = esp_wifi_scan_get_ap_records(&found, out);
        if (err == ESP_OK) {
            qsort(out, found, sizeof(out[0]), cmp_rssi);

            /* De-dup by SSID, keeping the strongest -- mesh and dual-band
             * networks otherwise fill the whole list with one name. */
            uint16_t w = 0;
            for (uint16_t i = 0; i < found; i++) {
                if (out[i].ssid[0] == '\0') continue;      /* hidden */
                bool dup = false;
                for (uint16_t j = 0; j < w; j++) {
                    if (strcmp((char *)out[j].ssid, (char *)out[i].ssid) == 0) {
                        dup = true; break;
                    }
                }
                if (!dup) out[w++] = out[i];
            }
            *n_inout = w;
        }
    }
    esp_wifi_scan_stop();

    s_scanning = false;
    if (s_want_connect) schedule_retry(200);
    return err;
}

void wifi_mgr_info(char *ip, size_t ip_cap, int8_t *rssi)
{
    if (ip && ip_cap) {
        ip[0] = '\0';
        esp_netif_ip_info_t info;
        if (s_netif && esp_netif_get_ip_info(s_netif, &info) == ESP_OK) {
            snprintf(ip, ip_cap, IPSTR, IP2STR(&info.ip));
        }
    }
    if (rssi) {
        *rssi = 0;
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) *rssi = ap.rssi;
    }
}
