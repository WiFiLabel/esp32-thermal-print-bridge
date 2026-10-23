#include "app_ota.h"

#include <stdint.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "app_restart.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "net_bringup.h"

static const char *TAG = "ota";

#define OTA_PORT 8080
#define OTA_CHUNK 4096
#define OTA_TOKEN_HDR "X-OTA-Token"
#define CONFIRM_DELAY_US (60ULL * 1000000ULL)
#define ROLLBACK_DELAY_US (180ULL * 1000000ULL)

#ifndef WIFILABEL_OTA_TOKEN
#define WIFILABEL_OTA_TOKEN ""
#endif

static bool s_busy;
static bool s_confirmed;
static bool s_confirm_armed;
static esp_timer_handle_t s_confirm_timer;
static esp_timer_handle_t s_rollback_timer;

static bool token_matches(const char *got, size_t got_len)
{
    const char *expect = WIFILABEL_OTA_TOKEN;
    size_t expect_len = strlen(expect);
    if (expect_len == 0 || got_len != expect_len) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < expect_len; i++) {
        diff |= (uint8_t)got[i] ^ (uint8_t)expect[i];
    }
    return diff == 0;
}

static bool request_authorized(httpd_req_t *req)
{
    if (WIFILABEL_OTA_TOKEN[0] == '\0') {
        return false;
    }
    size_t hdr_len = httpd_req_get_hdr_value_len(req, OTA_TOKEN_HDR);
    char hdr[128];
    if (hdr_len == 0 || hdr_len >= sizeof(hdr) ||
        httpd_req_get_hdr_value_str(req, OTA_TOKEN_HDR, hdr, sizeof(hdr)) != ESP_OK ||
        !token_matches(hdr, hdr_len)) {
        memset(hdr, 0, sizeof(hdr));
        return false;
    }
    memset(hdr, 0, sizeof(hdr));
    return true;
}

static esp_err_t reject_unauthorized(httpd_req_t *req)
{
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
    return ESP_FAIL;
}

bool app_ota_is_pending(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    return running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
}

static bool other_slot_has_image(void)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t desc;
    return next && esp_ota_get_partition_description(next, &desc) == ESP_OK;
}

static void confirm_pending(void)
{
    if (s_confirmed) {
        return;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        s_confirmed = true;
        return;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "confirm image: %s", esp_err_to_name(err));
        return;
    }
    s_confirmed = true;
    if (s_rollback_timer) {
        esp_timer_stop(s_rollback_timer);
    }
    ESP_LOGI(TAG, "running image confirmed on %s", running->label);
}

static bool rollback_allowed(void)
{
    return s_rollback_timer && !s_confirmed && app_ota_is_pending() && !net_in_setup() && other_slot_has_image();
}

static void stop_rollback(void)
{
    if (s_rollback_timer) {
        esp_timer_stop(s_rollback_timer);
    }
}

static void arm_rollback(void)
{
    if (!rollback_allowed() || esp_timer_is_active(s_rollback_timer)) {
        return;
    }
    if (esp_timer_start_once(s_rollback_timer, ROLLBACK_DELAY_US) != ESP_OK) {
        ESP_LOGE(TAG, "rollback timer did not start");
        return;
    }
    ESP_LOGI(TAG, "pending image rolls back after 3 minutes without an address");
}

static void confirm_timer_cb(void *arg)
{
    (void)arg;
    s_confirm_armed = false;
    if (!net_copy_ip(NULL, 0, NULL, 0)) {
        ESP_LOGI(TAG, "address dropped before the image was confirmed");
        arm_rollback();
        return;
    }
    confirm_pending();
}

static void rollback_timer_cb(void *arg)
{
    (void)arg;
    if (!rollback_allowed()) {
        return;
    }
    if (net_copy_ip(NULL, 0, NULL, 0)) {
        if (!s_confirm_armed && esp_timer_start_once(s_confirm_timer, CONFIRM_DELAY_US) == ESP_OK) {
            s_confirm_armed = true;
            ESP_LOGI(TAG, "will confirm this image after 60s with an address");
        }
        ESP_LOGI(TAG, "address is up; rollback waits for the confirm window");
        return;
    }
    ESP_LOGW(TAG, "no address for 3 minutes, rebooting into the previous image");
    esp_restart();
}

static void arm_pending_watch(void)
{
    const esp_timer_create_args_t confirm_args = {
        .callback = confirm_timer_cb,
        .name = "ota_confirm",
    };
    const esp_timer_create_args_t rollback_args = {
        .callback = rollback_timer_cb,
        .name = "ota_rollback",
    };
    if (esp_timer_create(&confirm_args, &s_confirm_timer) != ESP_OK) {
        ESP_LOGE(TAG, "confirm timer was not created");
        return;
    }
    if (!app_ota_is_pending()) {
        s_confirmed = true;
        return;
    }
    if (net_in_setup()) {
        ESP_LOGI(TAG, "setup mode: pending image waits for an address, no rollback reboot");
        return;
    }
    if (!other_slot_has_image()) {
        ESP_LOGI(TAG, "no previous image; pending image will not force a reboot");
        return;
    }
    if (esp_timer_create(&rollback_args, &s_rollback_timer) != ESP_OK) {
        ESP_LOGE(TAG, "rollback timer was not created");
    }
}

void app_ota_note_address(bool have)
{
    if (s_confirmed || !s_confirm_timer || !app_ota_is_pending()) {
        return;
    }
    if (!have) {
        if (s_confirm_armed) {
            esp_timer_stop(s_confirm_timer);
            s_confirm_armed = false;
        }
        arm_rollback();
        return;
    }
    stop_rollback();
    if (s_confirm_armed) {
        return;
    }
    if (esp_timer_start_once(s_confirm_timer, CONFIRM_DELAY_US) != ESP_OK) {
        ESP_LOGE(TAG, "confirm timer did not start");
        return;
    }
    s_confirm_armed = true;
    ESP_LOGI(TAG, "will confirm this image after 60s with an address");
}

static esp_err_t update_get(httpd_req_t *req)
{
    if (!request_authorized(req)) {
        ESP_LOGW(TAG, "status refused, bad token");
        return reject_unauthorized(req);
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t *app = esp_app_get_description();
    char body[384];
    snprintf(body, sizeof(body),
             "project %s\nversion %s\nbuilt %s %s\nrunning %s\nnext %s\n"
             "POST the application bin to this URL with header " OTA_TOKEN_HDR "\n",
             app ? app->project_name : "?", app ? app->version : "?", app ? app->date : "?", app ? app->time : "?",
             running ? running->label : "?", next ? next->label : "?");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t update_post(httpd_req_t *req)
{
    if (s_busy) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "update already running");
        return ESP_FAIL;
    }
    if (WIFILABEL_OTA_TOKEN[0] == '\0') {
        ESP_LOGW(TAG, "update refused, no token configured");
        return reject_unauthorized(req);
    }
    if (!request_authorized(req)) {
        ESP_LOGW(TAG, "update refused, bad token");
        return reject_unauthorized(req);
    }

    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "POST the .bin with Content-Length");
        return ESP_FAIL;
    }

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no update slot");
        return ESP_FAIL;
    }
    if ((size_t)req->content_len > next->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image larger than the update slot");
        return ESP_FAIL;
    }

    const size_t desc_off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    const size_t need = desc_off + sizeof(esp_app_desc_t);
    if ((size_t)req->content_len < need || need > OTA_CHUNK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image is not an application");
        return ESP_FAIL;
    }

    char buf[OTA_CHUNK];
    int primed = 0;
    while (primed < (int)need) {
        int n = httpd_req_recv(req, buf + primed, OTA_CHUNK - primed);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            ESP_LOGE(TAG, "upload stopped before the image header");
            return ESP_FAIL;
        }
        primed += n;
    }

    esp_app_desc_t incoming;
    memcpy(&incoming, buf + desc_off, sizeof(incoming));
    const esp_app_desc_t *running_desc = esp_app_get_description();
    if (!running_desc ||
        strncmp(incoming.project_name, running_desc->project_name, sizeof(incoming.project_name)) != 0) {
        ESP_LOGW(TAG, "update refused, project \"%s\"", incoming.project_name);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image is not this project");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "update %d bytes version %s -> %s", req->content_len, incoming.version, next->label);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota begin: %s", esp_err_to_name(err));
        const char *msg = (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) ? "running image is not confirmed yet"
                                                                       : "ota begin failed";
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
        return ESP_FAIL;
    }

    s_busy = true;
    err = esp_ota_write(handle, buf, (size_t)primed);
    if (err != ESP_OK) {
        esp_ota_abort(handle);
        s_busy = false;
        ESP_LOGE(TAG, "ota write: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write failed");
        return ESP_FAIL;
    }

    int left = req->content_len - primed;
    int got_total = primed;
    int next_log = 128 * 1024;
    while (left > 0) {
        int want = left > OTA_CHUNK ? OTA_CHUNK : left;
        int n = httpd_req_recv(req, buf, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            esp_ota_abort(handle);
            s_busy = false;
            ESP_LOGE(TAG, "upload stopped after %d bytes", got_total);
            return ESP_FAIL;
        }
        err = esp_ota_write(handle, buf, (size_t)n);
        if (err != ESP_OK) {
            esp_ota_abort(handle);
            s_busy = false;
            ESP_LOGE(TAG, "ota write: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write failed");
            return ESP_FAIL;
        }
        left -= n;
        got_total += n;
        if (got_total >= next_log) {
            ESP_LOGI(TAG, "update %d/%d", got_total, req->content_len);
            next_log += 128 * 1024;
        }
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        s_busy = false;
        ESP_LOGE(TAG, "ota end: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "image rejected");
        return ESP_FAIL;
    }
    err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) {
        s_busy = false;
        ESP_LOGE(TAG, "set boot: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not select the new image");
        return ESP_FAIL;
    }

    if (app_schedule_restart(500) != ESP_OK) {
        s_busy = false;
        ESP_LOGE(TAG, "could not schedule reboot");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not reboot");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "update stored in %s, rebooting", next->label);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "saved, rebooting\n");
    return ESP_OK;
}

static const httpd_uri_t s_get = {
    .uri = "/update",
    .method = HTTP_GET,
    .handler = update_get,
};

static const httpd_uri_t s_post = {
    .uri = "/update",
    .method = HTTP_POST,
    .handler = update_post,
};

esp_err_t app_ota_start(void)
{
    arm_pending_watch();
    if (app_ota_is_pending()) {
        app_ota_note_address(net_copy_ip(NULL, 0, NULL, 0));
    }
    if (WIFILABEL_OTA_TOKEN[0] == '\0') {
        ESP_LOGW(TAG, "app update is closed until a token is configured");
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = OTA_PORT;
    cfg.ctrl_port = 32769;
    cfg.max_open_sockets = 2;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 60;
    cfg.send_wait_timeout = 10;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http start: %s", esp_err_to_name(err));
        return err;
    }
    httpd_register_uri_handler(server, &s_get);
    httpd_register_uri_handler(server, &s_post);
    ESP_LOGI(TAG, "app update on TCP %d POST /update", OTA_PORT);
    return ESP_OK;
}
