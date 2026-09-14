#include "storage.h"

#include "esp_littlefs.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "storage";

static bool s_cfg_ok;
static bool s_data_ok;

/* One-shot "we have already formatted this" marker, so a factory-fresh
 * partition gets formatted but a previously-working one never does. */
#define NVS_NS       "prompanel"
#define KEY_DATA_FMT "data_fmt"

static bool data_formatted_before(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, KEY_DATA_FMT, &v);
    nvs_close(h);
    return err == ESP_OK && v != 0;
}

static void mark_data_formatted(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, KEY_DATA_FMT, 1);
    nvs_commit(h);
    nvs_close(h);
}

static bool mount_littlefs(const char *label, const char *base, bool format_on_fail)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path              = base,
        .partition_label        = label,
        .format_if_mount_failed = format_on_fail,
        .dont_mount             = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount %s at %s failed: %s", label, base, esp_err_to_name(err));
        return false;
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info(label, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "mounted %s at %s: %u/%u KB used",
                 label, base, (unsigned)(used / 1024), (unsigned)(total / 1024));
    }
    return true;
}

esp_err_t storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erasing (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* config: formatting on failure is correct -- an unreadable config
     * partition means starting over, and the alternative is a device that
     * cannot be configured at all. */
    s_cfg_ok = mount_littlefs("config", STORAGE_CFG_PATH, true);

    /*
     * data: holds cached history snapshots, so a mount failure must NOT
     * silently reformat -- losing hours of chart history to a transient fault
     * is worse than running without the cache.
     *
     * But a factory-fresh partition has never been formatted and would fail
     * that check forever, so the first failure is allowed exactly one format,
     * recorded in NVS. After that marker exists, a failure is a real failure
     * and we run degraded and say so.
     */
    s_data_ok = mount_littlefs("data", STORAGE_DATA_PATH, false);
    if (!s_data_ok) {
        if (!data_formatted_before()) {
            ESP_LOGI(TAG, "data partition is unformatted; formatting once");
            mark_data_formatted();
            s_data_ok = mount_littlefs("data", STORAGE_DATA_PATH, true);
        } else {
            ESP_LOGW(TAG, "%s failed to mount and has been formatted before; "
                          "NOT reformatting. Continuing without cached history.",
                     STORAGE_DATA_PATH);
        }
    }

    return s_cfg_ok ? ESP_OK : ESP_FAIL;
}

bool storage_cfg_ready(void)  { return s_cfg_ok; }
bool storage_data_ready(void) { return s_data_ok; }

void storage_usage(const char *base_path, size_t *total, size_t *used)
{
    if (total) *total = 0;
    if (used)  *used  = 0;

    const char *label = NULL;
    if (strcmp(base_path, STORAGE_CFG_PATH) == 0 && s_cfg_ok)        label = "config";
    else if (strcmp(base_path, STORAGE_DATA_PATH) == 0 && s_data_ok) label = "data";
    if (label == NULL) return;

    esp_littlefs_info(label, total, used);
}
