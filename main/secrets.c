#include "secrets.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>

static const char *TAG    = "secrets";
static const char *NVS_NS = "prompanel";

static const char *KEY_SSID = "wifi_ssid";
static const char *KEY_PASS = "wifi_pass";

static esp_err_t get_str(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    esp_err_t err = nvs_get_str(h, key, dst, &len);
    if (err != ESP_OK && cap > 0) dst[0] = '\0';
    return err;
}

bool secrets_have_wifi(void)
{
    char ssid[SECRETS_SSID_MAX] = "";
    if (secrets_get_wifi(ssid, sizeof(ssid), NULL, 0) != ESP_OK) return false;
    return ssid[0] != '\0';
}

esp_err_t secrets_get_wifi(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap)
{
    if (ssid && ssid_cap) ssid[0] = '\0';
    if (pass && pass_cap) pass[0] = '\0';

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;      /* namespace absent on a fresh device */

    if (ssid && ssid_cap) get_str(h, KEY_SSID, ssid, ssid_cap);
    if (pass && pass_cap) get_str(h, KEY_PASS, pass, pass_cap);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t secrets_set_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, KEY_SSID, ssid ? ssid : "");
    /*
     * A NULL password means "keep whatever is stored". That is what lets the
     * settings screen show an empty password box without an empty box meaning
     * "erase my password" -- the single most annoying way to lose a WPA
     * passphrase you typed on a touchscreen.
     */
    if (err == ESP_OK && pass != NULL) err = nvs_set_str(h, KEY_PASS, pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    /* Never log the password, and never log the SSID at anything above debug:
     * serial logs get pasted into issues. */
    ESP_LOGI(TAG, "wifi credentials %s", err == ESP_OK ? "stored" : "NOT stored");
    return err;
}

esp_err_t secrets_clear_wifi(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, KEY_SSID);
    nvs_erase_key(h, KEY_PASS);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void ep_key(uint16_t ep_id, char *out, size_t cap)
{
    snprintf(out, cap, "ep%u_auth", (unsigned)ep_id);
}

esp_err_t secrets_get_ep_auth(uint16_t ep_id, char *buf, size_t cap)
{
    if (buf && cap) buf[0] = '\0';
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    char key[16];
    ep_key(ep_id, key, sizeof(key));
    err = get_str(h, key, buf, cap);
    nvs_close(h);
    return err;
}

esp_err_t secrets_set_ep_auth(uint16_t ep_id, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    char key[16];
    ep_key(ep_id, key, sizeof(key));
    err = nvs_set_str(h, key, value ? value : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t secrets_del_ep_auth(uint16_t ep_id)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    char key[16];
    ep_key(ep_id, key, sizeof(key));
    nvs_erase_key(h, key);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t secrets_erase_all(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_all(h);
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "all secrets erased");
    return err;
}
