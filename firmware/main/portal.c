#include "portal.h"

#include <stdio.h>
#include <string.h>

#include "app_restart.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "net_bringup.h"
#include "nvs.h"
#include "portal_dns.h"
#include "web_ui.h"
#include "wifilabel_nvs.h"

static const char *TAG = "portal";

#define PORTAL_PAGE_MAX 4096
#define PORTAL_JOIN_FAILS_BEFORE_SETUP 4

static esp_netif_t *s_ap_netif;
static httpd_handle_t s_httpd;
static SemaphoreHandle_t s_mu;
static SemaphoreHandle_t s_page_mu;
static char s_page[PORTAL_PAGE_MAX];
static char s_saved_page[PORTAL_PAGE_MAX];
static char s_ap_ssid[33];
static char s_host[32];
static char s_captive_uri[48];
static uint32_t s_ap_ip;
static bool s_restarting;
static bool s_want_setup;
static bool s_cable;
static bool s_sta_has_ip;
static int s_join_fails;
static bool s_wifi_inited;
static bool s_radio_owned;
static bool s_wifi_running;
static bool s_ap_on;

static bool page_lock(void)
{
    return s_page_mu && xSemaphoreTake(s_page_mu, pdMS_TO_TICKS(3000)) == pdTRUE;
}

static void page_unlock(void)
{
    xSemaphoreGive(s_page_mu);
}

static void send_locked_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, s_page, HTTPD_RESP_USE_STRLEN);
}

static void make_ap_ssid(const char *hostname)
{
    const char *suf = strrchr(hostname ? hostname : "", '-');
    if (!suf || !suf[1]) {
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "WiFiLabel");
        return;
    }
    suf++;
    char up[8];
    size_t i = 0;
    for (; suf[i] && i < sizeof(up) - 1; i++) {
        char c = suf[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - ('a' - 'A'));
        }
        up[i] = c;
    }
    up[i] = '\0';
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "WiFiLabel-%s", up);
}

static void fill_ap_config(wifi_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    memcpy(cfg->ap.ssid, s_ap_ssid, strlen(s_ap_ssid));
    cfg->ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    cfg->ap.channel = 6;
    cfg->ap.authmode = WIFI_AUTH_OPEN;
    cfg->ap.max_connection = 4;
    cfg->ap.beacon_interval = 100;
}

static void set_captive_dhcp(void)
{
    for (int attempt = 0; attempt < 20; attempt++) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            char ip_s[16];
            inet_ntoa_r(ip.ip.addr, ip_s, sizeof(ip_s));
            snprintf(s_captive_uri, sizeof(s_captive_uri), "http://%s/", ip_s);
            s_ap_ip = ip.ip.addr;
            esp_err_t err = esp_netif_dhcps_stop(s_ap_netif);
            if (err == ESP_OK || err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
                err = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, s_captive_uri,
                                             strlen(s_captive_uri));
                esp_netif_dhcps_start(s_ap_netif);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "setup network %s at %s (open, no password)", s_ap_ssid, ip_s);
                    return;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGW(TAG, "DHCP captive URI was not set");
}

static void saved_ssid(char *out, size_t out_len)
{
    wifilabel_nvs_get_str("ssid", out, out_len);
}

static bool build_page(const char *notice)
{
    char current[33];
    saved_ssid(current, sizeof(current));

    web_ui_out_t out;
    web_ui_out_init(&out, s_page, sizeof(s_page));
    web_ui_open(&out);
    web_ui_text(&out, "<h1>WiFiLabel</h1>"
                      "<p class=\"sub\">Connect this Bridge to Wi-Fi.</p>");
    if (notice && notice[0]) {
        web_ui_text(&out, "<p class=\"banner\">");
        web_ui_html(&out, notice);
        web_ui_text(&out, "</p>");
    }
    if (current[0]) {
        web_ui_text(&out, "<p class=\"banner\">Saved network that has not connected: ");
        web_ui_html(&out, current);
        web_ui_text(&out, "</p>");
    }
    web_ui_text(&out, "<form method=\"POST\" action=\"/join\" autocomplete=\"off\">"
                      "<label>Network name"
                      "<input name=\"ssid\" maxlength=\"32\" autocomplete=\"off\"></label>"
                      "<label>Password"
                      "<input name=\"pass\" type=\"password\" maxlength=\"63\" autocomplete=\"off\"></label>"
                      "<p class=\"hint\">Leave the password empty for an open network. "
                      "Otherwise it needs 8 or more characters.</p>"
                      "<button type=\"submit\">Save</button></form>");
    web_ui_close(&out, "setup");
    return !out.full;
}

static void send_page(httpd_req_t *req, const char *notice)
{
    if (!page_lock()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
        return;
    }
    bool ok = build_page(notice);
    if (ok) {
        send_locked_page(req);
    }
    page_unlock();
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
    }
}

static esp_err_t root_get(httpd_req_t *req)
{
    send_page(req, NULL);
    return ESP_OK;
}

static esp_err_t redirect_root(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    /* iOS treats an empty redirect as a failure and hides the sheet. */
    httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode(char *s)
{
    char *r = s;
    char *w = s;
    while (*r) {
        if (*r == '+') {
            *w++ = ' ';
            r++;
        } else if (*r == '%' && hex_nibble(r[1]) >= 0 && hex_nibble(r[2]) >= 0) {
            *w++ = (char)((hex_nibble(r[1]) << 4) | hex_nibble(r[2]));
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static bool form_get(char *body, const char *key, char *out, size_t out_len)
{
    size_t klen = strlen(key);
    char *p = body;
    out[0] = '\0';
    while (p && *p) {
        char *amp = strchr(p, '&');
        if (amp) {
            *amp = '\0';
        }
        bool match = strncmp(p, key, klen) == 0 && p[klen] == '=';
        if (match) {
            snprintf(out, out_len, "%s", p + klen + 1);
            url_decode(out);
        }
        if (amp) {
            *amp = '&';
            p = amp + 1;
        } else {
            break;
        }
        if (match) {
            return true;
        }
    }
    return false;
}

static bool text_is_printable(const char *s, size_t max_len)
{
    size_t n = strlen(s);
    if (n > max_len) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)s[i] < 32) {
            return false;
        }
    }
    return true;
}

static esp_err_t join_post(httpd_req_t *req)
{
    if (s_restarting) {
        httpd_resp_send(req, "Restarting", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > 400) {
        send_page(req, "That form was empty or too long.");
        return ESP_OK;
    }
    char body[401];
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = '\0';

    net_wifi_creds_t creds = {0};
    form_get(body, "ssid", creds.ssid, sizeof(creds.ssid));
    form_get(body, "pass", creds.pass, sizeof(creds.pass));

    if (creds.ssid[0] == '\0' || !text_is_printable(creds.ssid, 32)) {
        send_page(req, "Type a network name.");
        return ESP_OK;
    }
    size_t pass_len = strlen(creds.pass);
    if (pass_len > 63 || !text_is_printable(creds.pass, 63) || (pass_len > 0 && pass_len < 8)) {
        send_page(req, "Password must be at least 8 characters, or empty for an open network.");
        return ESP_OK;
    }

    nvs_handle_t h = 0;
    bool opened = false;
    /* One commit so the network and the welcome request land together. */
    esp_err_t err = nvs_open("wifilabel", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        opened = true;
        err = nvs_set_str(h, "ssid", creds.ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "pass", creds.pass);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "welcome", 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (opened) {
        nvs_close(h);
    }
    if (err != ESP_OK) {
        send_page(req, "Could not save. Try again.");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "saved network \"%s\", restarting into station mode", creds.ssid);
    bool locked = page_lock();
    char *storage = locked ? s_page : s_saved_page;
    web_ui_out_t out;
    web_ui_out_init(&out, storage, PORTAL_PAGE_MAX);
    web_ui_open(&out);
    web_ui_text(&out, "<h1>Saved</h1><p>The Bridge is joining ");
    web_ui_html(&out, creds.ssid);
    web_ui_text(&out, ". This page turns off in a moment.</p>");
    web_ui_close(&out, "saved");
    if (!out.full) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_send(req, storage, HTTPD_RESP_USE_STRLEN);
    } else {
        ESP_LOGW(TAG, "saved page did not fit");
    }
    if (locked) {
        page_unlock();
    }
    s_restarting = true;
    app_schedule_restart(800);
    return ESP_OK;
}

static const httpd_uri_t s_root = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get,
};

static const httpd_uri_t s_join = {
    .uri = "/join",
    .method = HTTP_POST,
    .handler = join_post,
};

static esp_err_t start_http(void)
{
    if (!s_mu) {
        s_mu = xSemaphoreCreateMutex();
        if (!s_mu) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_page_mu) {
        s_page_mu = xSemaphoreCreateMutex();
        if (!s_page_mu) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_httpd) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http start: %s", esp_err_to_name(err));
        s_httpd = NULL;
        return err;
    }
    httpd_register_uri_handler(s_httpd, &s_root);
    httpd_register_uri_handler(s_httpd, &s_join);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, redirect_root);
    ESP_LOGI(TAG, "setup page on port 80");
    return ESP_OK;
}

static esp_err_t ensure_ap_netif(void)
{
    make_ap_ssid(s_host);
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            return ESP_ERR_NO_MEM;
        }
        if (s_host[0]) {
            esp_netif_set_hostname(s_ap_netif, s_host);
        }
    }
    return ESP_OK;
}

static esp_err_t start_ap(void)
{
    if (s_ap_on) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ensure_ap_netif(), TAG, "ap netif");
    wifi_config_t ap;
    fill_ap_config(&ap);
    if (!s_wifi_inited) {
        esp_netif_t *sta = esp_netif_create_default_wifi_sta();
        if (!sta) {
            return ESP_ERR_NO_MEM;
        }
        if (s_host[0]) {
            esp_netif_set_hostname(sta, s_host);
        }
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
        s_wifi_inited = true;
        s_radio_owned = true;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap config");
    if (!s_wifi_running) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
        s_wifi_running = true;
    }
    set_captive_dhcp();
    if (s_ap_ip == 0) {
        ESP_LOGW(TAG, "setup address not ready");
        return ESP_FAIL;
    }
    esp_err_t err = portal_dns_start(s_ap_ip);
    if (err != ESP_OK) {
        return err;
    }
    s_ap_on = true;
    return ESP_OK;
}

static void stop_ap(void)
{
    if (!s_ap_on) {
        return;
    }
    portal_dns_stop();
    if (s_radio_owned) {
        esp_wifi_stop();
        s_wifi_running = false;
    } else if (s_wifi_running) {
        esp_wifi_set_mode(WIFI_MODE_STA);
    }
    s_ap_on = false;
    ESP_LOGI(TAG, "setup network off");
}

static bool page_needed(void)
{
    if (s_want_setup) {
        return true;
    }
    return s_join_fails >= PORTAL_JOIN_FAILS_BEFORE_SETUP && !s_cable && !s_sta_has_ip;
}

static bool ap_needed(void)
{
    return page_needed() && !s_cable && !s_sta_has_ip;
}

static void reconcile(void)
{
    if (!s_mu) {
        s_mu = xSemaphoreCreateMutex();
        if (!s_mu) {
            return;
        }
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return;
    }
    if (page_needed()) {
        start_http();
    }
    if (ap_needed()) {
        if (start_ap() != ESP_OK) {
            ESP_LOGE(TAG, "setup network did not start");
        }
    } else {
        stop_ap();
    }
    xSemaphoreGive(s_mu);
}

static void defer_ap_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    if (!s_cable && s_want_setup && !s_sta_has_ip) {
        reconcile();
    }
    vTaskDelete(NULL);
}

esp_err_t portal_arm_setup(const char *hostname)
{
    snprintf(s_host, sizeof(s_host), "%s", hostname ? hostname : "");
    s_want_setup = true;
    esp_err_t err = start_http();
    if (err != ESP_OK) {
        return err;
    }
    if (s_cable) {
        ESP_LOGI(TAG, "cable is up, setup page stays on Ethernet");
        return ESP_OK;
    }
    if (xTaskCreate(defer_ap_task, "portal_ap", 4096, NULL, 5, NULL) != pdPASS) {
        return start_ap();
    }
    return ESP_OK;
}

void portal_note_cable(bool link_up)
{
    s_cable = link_up;
    if (s_want_setup || s_ap_on || s_join_fails >= PORTAL_JOIN_FAILS_BEFORE_SETUP) {
        reconcile();
    }
}

void portal_note_sta(bool have_ip)
{
    s_sta_has_ip = have_ip;
    if (have_ip) {
        s_join_fails = 0;
    }
    if (s_ap_on || s_want_setup) {
        reconcile();
    }
}

void portal_note_join_failed(void)
{
    if (s_sta_has_ip) {
        return;
    }
    s_join_fails++;
    ESP_LOGW(TAG, "Wi-Fi join failed, attempt %d", s_join_fails);
    if (!s_cable && s_join_fails >= PORTAL_JOIN_FAILS_BEFORE_SETUP) {
        reconcile();
    }
}

void portal_note_radio_started(void)
{
    s_wifi_inited = true;
    s_wifi_running = true;
    s_radio_owned = false;
}
