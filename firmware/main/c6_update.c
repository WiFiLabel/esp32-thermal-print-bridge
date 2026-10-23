#include "c6_update.h"

#include <stdbool.h>
#include <string.h>

#include "eh_host_cp_ota.h"
#include "eh_host_sys.h"
#include "esp_log.h"
#include "esp_system.h"
#include "app_ota.h"
#include "wifilabel_nvs.h"

extern const uint8_t c6_fw_start[] asm("_binary_eh_cp_wifi_sta_bin_start");
extern const uint8_t c6_fw_end[] asm("_binary_eh_cp_wifi_sta_bin_end");

static const char *TAG = "c6";

/* Image built from espressif/esp_hosted 3.0.10, SDIO stream mode. */
#define C6_WANT_MAJOR 3
#define C6_WANT_MINOR 0
#define C6_WANT_PATCH 10
#define C6_CHUNK 1400
#define C6_MAX_TRIES 2

static bool version_matches(const eh_host_coprocessor_fwver_t *ver)
{
    return ver->major1 == C6_WANT_MAJOR && ver->minor1 == C6_WANT_MINOR && ver->patch1 == C6_WANT_PATCH;
}

static uint8_t tries_load(void)
{
    uint8_t tries = 0;
    wifilabel_nvs_get_u8("c6try", &tries);
    return tries;
}

static void tries_write(uint8_t tries, bool erase)
{
    if (erase) {
        wifilabel_nvs_erase("c6try");
    } else {
        wifilabel_nvs_set_u8("c6try", tries);
    }
}

esp_err_t c6_update_if_needed(void)
{
    eh_host_coprocessor_fwver_t ver = {0};
    esp_err_t err = eh_host_sys_get_cp_fw_version(&ver);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "coprocessor firmware %u.%u.%u", (unsigned)ver.major1, (unsigned)ver.minor1,
                 (unsigned)ver.patch1);
        if (version_matches(&ver)) {
            tries_write(0, true);
            return ESP_OK;
        }
    } else {
        ESP_LOGW(TAG, "coprocessor version query failed: %s", esp_err_to_name(err));
    }

    const size_t image_len = (size_t)(c6_fw_end - c6_fw_start);
    if (image_len < 16) {
        ESP_LOGE(TAG, "embedded C6 image is empty");
        return ESP_ERR_INVALID_SIZE;
    }

    if (app_ota_is_pending()) {
        ESP_LOGW(TAG, "app image is not confirmed yet; C6 update waits for the next boot");
        return ESP_OK;
    }

    uint8_t tries = tries_load();
    if (tries >= C6_MAX_TRIES) {
        ESP_LOGE(TAG, "C6 update failed %u times; strap C6_IO9 low and flash C6_U0TXD/C6_U0RXD with the P4 in download mode",
                 (unsigned)tries);
        return ESP_FAIL;
    }
    tries_write((uint8_t)(tries + 1), false);

    ESP_LOGI(TAG, "updating C6 over SDIO (%u bytes)", (unsigned)image_len);
    err = eh_host_cp_ota_begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "C6 OTA begin: %s", esp_err_to_name(err));
        return err;
    }

    size_t off = 0;
    unsigned chunks = 0;
    while (off < image_len) {
        size_t n = image_len - off;
        if (n > C6_CHUNK) {
            n = C6_CHUNK;
        }
        err = eh_host_cp_ota_write(c6_fw_start + off, (uint32_t)n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "C6 OTA write at %u: %s", (unsigned)off, esp_err_to_name(err));
            ESP_LOGE(TAG, "restarting so the next boot can retry the C6 update");
            esp_restart();
        }
        off += n;
        chunks++;
        if ((chunks % 64) == 0) {
            ESP_LOGI(TAG, "C6 OTA %u/%u", (unsigned)off, (unsigned)image_len);
        }
    }

    err = eh_host_cp_ota_end();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "C6 OTA end: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "restarting so the next boot can retry the C6 update");
        esp_restart();
    }
    err = eh_host_cp_ota_activate();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "C6 OTA activate: %s (restarting anyway)", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "C6 image activated, restarting");
    }
    esp_restart();
}
