#include "net_bringup.h"

#include <stdio.h>
#include <string.h>

#include "app_ota.h"
#include "portal.h"
#include "printer_id.h"
#include "usb_printer.h"
#include "wifilabel_nvs.h"

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "mdns.h"
#include "sdkconfig.h"

static const char *TAG = "net";

/* Waveshare ESP32-P4-NANO: IP101GRI RMII, same map as the IDF P4 EMAC defaults. */
#define ETH_PHY_ADDR 1
#define ETH_PHY_RST_GPIO 51
#define ETH_MDC_GPIO 31
#define ETH_MDIO_GPIO 52
#define ETH_REF_CLK_GPIO 50
#define ETH_ROUTE_PRIO 200
#define WIFI_BACKOFF_MIN_MS 1000
#define WIFI_BACKOFF_MAX_MS 30000

static esp_netif_t *s_eth_netif;
static esp_netif_t *s_wifi_netif;
static esp_eth_handle_t s_eth_handle;
static SemaphoreHandle_t s_mu;
static net_ip_cb_t s_ip_cb;
static bool s_eth_up;
static bool s_wifi_up;
static esp_ip4_addr_t s_eth_ip;
static esp_ip4_addr_t s_wifi_ip;
static char s_hostname[32];
static char s_instance[64];
static char s_suffix[8];
static bool s_mdns_up;
static char s_mdns_adminurl[96];
static char s_mdns_uuid[48];
static char s_txt_note[128];
static char s_txt_product_ipp[80];
static char s_txt_product_raw[128];
static char s_txt_mfg[48];
static char s_txt_mdl[64];
static char s_txt_cmd[96];
static char s_ip[16];
static char s_iface[8];
static bool s_have_ip;
static bool s_setup_mode;
static bool s_sta_joined;
static bool s_eth_link;
static esp_timer_handle_t s_wifi_retry;
static uint32_t s_wifi_backoff_ms = WIFI_BACKOFF_MIN_MS;
static bool s_wifi_retry_armed;

static void publish_locked(void)
{
    const esp_ip4_addr_t *ip = NULL;
    const char *iface = NULL;
    if (s_eth_up && s_eth_ip.addr != 0) {
        ip = &s_eth_ip;
        iface = "eth";
    } else if (s_wifi_up && s_wifi_ip.addr != 0) {
        ip = &s_wifi_ip;
        iface = "wifi";
    }

    bool had = s_have_ip;
    char prev[16];
    memcpy(prev, s_ip, sizeof(prev));

    if (!ip) {
        s_have_ip = false;
        s_ip[0] = '\0';
        s_iface[0] = '\0';
    } else {
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(ip));
        snprintf(s_iface, sizeof(s_iface), "%s", iface);
        s_have_ip = true;
    }

    bool changed = had != s_have_ip || strcmp(prev, s_ip) != 0;
    net_ip_cb_t cb = s_ip_cb;
    if (changed) {
        if (s_have_ip) {
            ESP_LOGI(TAG, "preferred %s %s (%s.local)", s_iface, s_ip, s_hostname);
        } else {
            ESP_LOGW(TAG, "no IP address");
        }
    }
    xSemaphoreGive(s_mu);
    if (changed && cb) {
        cb();
    }
}

static void note_ip(esp_netif_t *netif, const esp_ip4_addr_t *addr, bool up)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (netif == s_eth_netif) {
        s_eth_up = up && addr && addr->addr != 0;
        if (s_eth_up) {
            s_eth_ip = *addr;
        } else {
            s_eth_ip.addr = 0;
        }
    } else if (netif == s_wifi_netif) {
        s_wifi_up = up && addr && addr->addr != 0;
        if (s_wifi_up) {
            s_wifi_ip = *addr;
            s_sta_joined = true;
        } else {
            s_wifi_ip.addr = 0;
            s_sta_joined = false;
        }
    }
    publish_locked();
}

static void publish_address(esp_netif_t *netif, bool have)
{
    app_ota_note_address(net_copy_ip(NULL, 0, NULL, 0));
    if (netif == s_wifi_netif) {
        portal_note_sta(have);
    } else if (netif == s_eth_netif && have) {
        portal_note_cable(true);
    }
}

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "Ethernet link up");
        s_eth_link = true;
        portal_note_cable(true);
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "Ethernet link down");
        s_eth_link = false;
        note_ip(s_eth_netif, NULL, false);
        publish_address(s_eth_netif, false);
        portal_note_cable(false);
    }
}

static void wifi_retry_cb(void *arg)
{
    (void)arg;
    s_wifi_retry_armed = false;
    if (s_setup_mode) {
        return;
    }
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi connect: %s", esp_err_to_name(err));
    }
}

static void wifi_backoff_reset(void)
{
    s_wifi_backoff_ms = WIFI_BACKOFF_MIN_MS;
    if (s_wifi_retry_armed && s_wifi_retry) {
        esp_timer_stop(s_wifi_retry);
        s_wifi_retry_armed = false;
    }
}

static void wifi_schedule_retry(void)
{
    if (s_setup_mode || s_wifi_retry_armed) {
        return;
    }
    if (!s_wifi_retry || esp_timer_start_once(s_wifi_retry, (uint64_t)s_wifi_backoff_ms * 1000) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi retry timer did not start");
        return;
    }
    s_wifi_retry_armed = true;
    ESP_LOGI(TAG, "Wi-Fi retry in %u ms", (unsigned)s_wifi_backoff_ms);
    uint32_t next = s_wifi_backoff_ms * 2;
    if (next > WIFI_BACKOFF_MAX_MS) {
        next = WIFI_BACKOFF_MAX_MS;
    }
    s_wifi_backoff_ms = next;
}

static bool wifi_reason_is_join_failure(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_CONNECTION_FAIL:
    case WIFI_REASON_ASSOC_FAIL:
        return true;
    default:
        return false;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    note_ip(event->esp_netif, &event->ip_info.ip, true);
    if (id == IP_EVENT_STA_GOT_IP) {
        wifi_backoff_reset();
    }
    publish_address(event->esp_netif, true);
}

static void on_lost_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    note_ip(event->esp_netif, NULL, false);
    publish_address(event->esp_netif, false);
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = data;
        ESP_LOGI(TAG, "setup: station " MACSTR " joined", MAC2STR(event->mac));
        return;
    }
    if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = data;
        ESP_LOGI(TAG, "setup: station " MACSTR " left, reason %d", MAC2STR(event->mac), event->reason);
        return;
    }
    if (s_setup_mode) {
        return;
    }
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = data;
        bool was_joined = s_sta_joined;
        note_ip(s_wifi_netif, NULL, false);
        publish_address(s_wifi_netif, false);
        if (event->reason != WIFI_REASON_ASSOC_LEAVE && !was_joined && !s_eth_link &&
            wifi_reason_is_join_failure(event->reason)) {
            portal_note_join_failed();
        }
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason %u", (unsigned)event->reason);
        wifi_schedule_retry();
    }
}

static void load_wifi_creds(net_wifi_creds_t *creds)
{
    creds->ssid[0] = '\0';
    creds->pass[0] = '\0';
    wifilabel_nvs_get_str("ssid", creds->ssid, sizeof(creds->ssid));
    wifilabel_nvs_get_str("pass", creds->pass, sizeof(creds->pass));
    if (creds->ssid[0] == '\0') {
        snprintf(creds->ssid, sizeof(creds->ssid), "%s", CONFIG_WIFILABEL_WIFI_SSID);
        snprintf(creds->pass, sizeof(creds->pass), "%s", CONFIG_WIFILABEL_WIFI_PASSWORD);
    }
}

static void fit_instance(char *dst, size_t dst_len, const char *model, bool real)
{
    char brand[24];
    snprintf(brand, sizeof(brand), "WiFiLabel-%s", s_suffix[0] ? s_suffix : "0000");
    printer_id_format_name(dst, dst_len, brand, real ? model : NULL);
}

bool net_copy_printer_label(char *dst, size_t dst_len)
{
    usb_printer_identity_t id;
    bool real = false;
    const char *base = "WiFiLabel";
    if (usb_printer_copy_identity(&id)) {
        base = printer_id_name_base(&id, &real);
    }
    if (dst && dst_len) {
        fit_instance(dst, dst_len, base, real);
    }
    return real;
}

static void fill_note(const usb_printer_identity_t *id, bool real)
{
    if (real && id->firmware[0]) {
        const char *model = id->mdl[0] ? id->mdl : id->mfg;
        snprintf(s_txt_note, sizeof(s_txt_note), "%s %s", model, id->firmware);
    } else if (real) {
        snprintf(s_txt_note, sizeof(s_txt_note), "%s", id->mdl[0] ? id->mdl : id->mfg);
    } else {
        snprintf(s_txt_note, sizeof(s_txt_note), "4x6 label printer");
    }
}

static void publish_mdns(void)
{
    if (!s_mdns_up) {
        return;
    }

    usb_printer_identity_t id;
    memset(&id, 0, sizeof(id));
    usb_printer_copy_identity(&id);

    bool real = false;
    const char *base = printer_id_name_base(&id, &real);
    fit_instance(s_instance, sizeof(s_instance), base, real);
    const char *ty = s_instance;
    fill_note(&id, real);

    snprintf(s_txt_product_ipp, sizeof(s_txt_product_ipp), "(%s)", s_instance);
    if (id.zpl) {
        snprintf(s_txt_product_raw, sizeof(s_txt_product_raw), "(ZPL Label Printer)");
    } else if (id.mfg[0] && id.mdl[0]) {
        snprintf(s_txt_product_raw, sizeof(s_txt_product_raw), "(%s %s)", id.mfg, id.mdl);
    } else {
        snprintf(s_txt_product_raw, sizeof(s_txt_product_raw), "(%s)", s_instance);
    }
    snprintf(s_txt_mfg, sizeof(s_txt_mfg), "%s", id.mfg);
    snprintf(s_txt_mdl, sizeof(s_txt_mdl), "%s", id.mdl);
    snprintf(s_txt_cmd, sizeof(s_txt_cmd), "%s", id.cmd);

    mdns_instance_name_set(s_instance);
    if (mdns_service_exists("_ipp", "_tcp", NULL)) {
        mdns_service_remove("_ipp", "_tcp");
    }
    if (mdns_service_exists("_pdl-datastream", "_tcp", NULL)) {
        mdns_service_remove("_pdl-datastream", "_tcp");
    }

    mdns_txt_item_t ipp_txt[] = {
        {"txtvers", "1"},
        {"qtotal", "1"},
        {"rp", "ipp/print"},
        {"ty", ty},
        {"product", s_txt_product_ipp},
        {"pdl", "image/pwg-raster,image/urf"},
        {"note", s_txt_note},
        {"priority", "0"},
        {"Color", "F"},
        {"Duplex", "F"},
        {"Copies", "T"},
        {"Transparent", "T"},
        {"Binary", "T"},
        {"printer-type", "0x1044"},
        {"URF", "V1.4,SRGB24,RS203,DM1,OB9"},
        {"UUID", s_mdns_uuid},
        {"adminurl", s_mdns_adminurl},
    };

    esp_err_t err = mdns_service_add(s_instance, "_ipp", "_tcp", 631, ipp_txt, sizeof(ipp_txt) / sizeof(ipp_txt[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns _ipp._tcp: %s", esp_err_to_name(err));
    } else {
        err = mdns_service_subtype_add_for_host(s_instance, "_ipp", "_tcp", NULL, "_universal");
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "mdns _universal subtype: %s", esp_err_to_name(err));
        }
        err = mdns_service_subtype_add_for_host(s_instance, "_ipp", "_tcp", NULL, "_print");
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "mdns _print subtype: %s", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "mDNS %s._ipp._tcp port 631", s_instance);
    }

    mdns_txt_item_t raw_txt[10];
    size_t raw_n = 0;
    raw_txt[raw_n++] = (mdns_txt_item_t){"txtvers", "1"};
    raw_txt[raw_n++] = (mdns_txt_item_t){"qtotal", "1"};
    raw_txt[raw_n++] = (mdns_txt_item_t){"ty", ty};
    raw_txt[raw_n++] = (mdns_txt_item_t){"product", s_txt_product_raw};
    raw_txt[raw_n++] = (mdns_txt_item_t){"pdl", "application/octet-stream"};
    raw_txt[raw_n++] = (mdns_txt_item_t){"note", s_txt_note};
    raw_txt[raw_n++] = (mdns_txt_item_t){"priority", "50"};
    if (s_txt_mfg[0]) {
        raw_txt[raw_n++] = (mdns_txt_item_t){"usb_MFG", s_txt_mfg};
    }
    if (s_txt_mdl[0]) {
        raw_txt[raw_n++] = (mdns_txt_item_t){"usb_MDL", s_txt_mdl};
    }
    if (s_txt_cmd[0]) {
        raw_txt[raw_n++] = (mdns_txt_item_t){"usb_CMD", s_txt_cmd};
    }
    err = mdns_service_add(s_instance, "_pdl-datastream", "_tcp", 9100, raw_txt, raw_n);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns _pdl-datastream._tcp: %s", esp_err_to_name(err));
    }
}

static void on_printer_identity(bool ready)
{
    (void)ready;
    publish_mdns();
}

static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed");
        return;
    }
    mdns_hostname_set(s_hostname);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_ETH);
    snprintf(s_mdns_adminurl, sizeof(s_mdns_adminurl), "http://%s.local:631/", s_hostname);
    snprintf(s_mdns_uuid, sizeof(s_mdns_uuid), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (uint8_t)0x40, (uint8_t)0x00, (uint8_t)0x80, (uint8_t)0x00,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    s_mdns_up = true;
    publish_mdns();
}

static esp_err_t start_ethernet(void)
{
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = ETH_PHY_RST_GPIO;
    phy_config.post_hw_reset_delay_ms = 200;

    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac.smi_gpio.mdc_num = ETH_MDC_GPIO;
    emac.smi_gpio.mdio_num = ETH_MDIO_GPIO;
    emac.interface = EMAC_DATA_INTERFACE_RMII;
    emac.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac.clock_config.rmii.clock_gpio = ETH_REF_CLK_GPIO;
#if SOC_EMAC_USE_MULTI_IO_MUX || SOC_EMAC_MII_USE_GPIO_MATRIX
    emac.emac_dataif_gpio.rmii.tx_en_num = 49;
    emac.emac_dataif_gpio.rmii.txd0_num = 34;
    emac.emac_dataif_gpio.rmii.txd1_num = 35;
    emac.emac_dataif_gpio.rmii.crs_dv_num = 28;
    emac.emac_dataif_gpio.rmii.rxd0_num = 29;
    emac.emac_dataif_gpio.rmii.rxd1_num = 30;
#endif

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac, &mac_config);
    if (!mac) {
        ESP_LOGE(TAG, "EMAC create failed");
        return ESP_FAIL;
    }
    /* IP101 is IEEE 802.3; IDF 6 exposes it through the generic PHY driver. */
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (!phy) {
        mac->del(mac);
        ESP_LOGE(TAG, "PHY create failed");
        return ESP_FAIL;
    }

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&config, &s_eth_handle);
    if (err != ESP_OK) {
        phy->del(phy);
        mac->del(mac);
        ESP_LOGE(TAG, "ethernet install: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    if (!s_eth_netif) {
        return ESP_ERR_NO_MEM;
    }
    esp_netif_set_hostname(s_eth_netif, s_hostname);
    /* Default Ethernet route priority is below Wi-Fi. Raise it so a cable wins. */
    esp_netif_set_route_prio(s_eth_netif, ETH_ROUTE_PRIO);

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth_handle);
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_eth_netif, glue), TAG, "eth attach failed");
    ESP_RETURN_ON_ERROR(esp_eth_start(s_eth_handle), TAG, "eth start failed");
    ESP_LOGI(TAG, "Ethernet started (IP101 addr %d, route prio %d)", ETH_PHY_ADDR, ETH_ROUTE_PRIO);
    return ESP_OK;
}

static esp_err_t start_wifi(void)
{
    net_wifi_creds_t creds;
    load_wifi_creds(&creds);
    if (creds.ssid[0] == '\0') {
        s_setup_mode = true;
        esp_err_t err = portal_arm_setup(s_hostname);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "setup page: %s", esp_err_to_name(err));
        }
        return err;
    }

    s_wifi_netif = esp_netif_create_default_wifi_sta();
    if (!s_wifi_netif) {
        return ESP_FAIL;
    }
    esp_netif_set_hostname(s_wifi_netif, s_hostname);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init (hosted C6): %s", esp_err_to_name(err));
        return err;
    }

    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, creds.ssid, strlen(creds.ssid));
    memcpy(wifi_config.sta.password, creds.pass, strlen(creds.pass));
    wifi_config.sta.threshold.authmode = creds.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    if (creds.pass[0]) {
        wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    }
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "wifi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    portal_note_radio_started();
    ESP_LOGI(TAG, "Wi-Fi STA started (fallback if Ethernet has no address)");
    return ESP_OK;
}

static void make_hostname(void)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_ETH) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_BASE);
    }
    snprintf(s_hostname, sizeof(s_hostname), "wifilabel-%02x%02x", mac[4], mac[5]);
    snprintf(s_suffix, sizeof(s_suffix), "%02X%02X", mac[4], mac[5]);
    snprintf(s_instance, sizeof(s_instance), "WiFiLabel-%s", s_suffix);
}

void net_set_ip_cb(net_ip_cb_t cb)
{
    s_ip_cb = cb;
}

const char *net_hostname(void)
{
    return s_hostname[0] ? s_hostname : "wifilabel";
}

bool net_in_setup(void)
{
    return s_setup_mode;
}

bool net_copy_ip(char *ip, size_t ip_len, char *iface, size_t iface_len)
{
    if (!s_mu) {
        return false;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool ok = s_have_ip && s_ip[0] != '\0';
    if (ok && ip && ip_len) {
        snprintf(ip, ip_len, "%s", s_ip);
    }
    if (ok && iface && iface_len) {
        snprintf(iface, iface_len, "%s", s_iface);
    }
    xSemaphoreGive(s_mu);
    return ok;
}

static void wifi_retry_timer_create(void)
{
    const esp_timer_create_args_t args = {
        .callback = wifi_retry_cb,
        .name = "wifi_retry",
    };
    if (esp_timer_create(&args, &s_wifi_retry) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi retry timer was not created");
    }
}

esp_err_t net_bringup_start(void)
{
    s_mu = xSemaphoreCreateMutex();
    if (!s_mu) {
        return ESP_ERR_NO_MEM;
    }
    make_hostname();
    wifi_retry_timer_create();
    ESP_LOGI(TAG, "hostname %s.local", s_hostname);

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &on_eth_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_got_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, &on_lost_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &on_lost_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi, NULL));

    esp_err_t eth_err = start_ethernet();
    if (eth_err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet unavailable (%s); Wi-Fi can still be used", esp_err_to_name(eth_err));
    }
    esp_err_t wifi_err = start_wifi();
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi unavailable (%s)", esp_err_to_name(wifi_err));
    }
    usb_printer_add_hotplug_cb(on_printer_identity);
    start_mdns();
    return ESP_OK;
}
