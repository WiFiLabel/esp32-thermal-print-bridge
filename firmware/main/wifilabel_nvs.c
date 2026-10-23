#include "wifilabel_nvs.h"

#include "nvs.h"

static esp_err_t open_ns(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    return nvs_open("wifilabel", mode, handle);
}

esp_err_t wifilabel_nvs_get_u8(const char *key, uint8_t *value)
{
    nvs_handle_t handle;
    esp_err_t err = open_ns(NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_u8(handle, key, value);
    nvs_close(handle);
    return err;
}

esp_err_t wifilabel_nvs_set_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle;
    esp_err_t err = open_ns(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t wifilabel_nvs_erase(const char *key)
{
    nvs_handle_t handle;
    esp_err_t err = open_ns(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t wifilabel_nvs_get_str(const char *key, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    nvs_handle_t handle;
    esp_err_t err = open_ns(NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t n = out_len;
    err = nvs_get_str(handle, key, out, &n);
    nvs_close(handle);
    if (err != ESP_OK) {
        out[0] = '\0';
    }
    return err;
}
