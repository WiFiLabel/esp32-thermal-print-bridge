#include "usb_printer.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

static const char *TAG = "usb_printer";

/* USB Printer Class 1.1 class-specific requests (bmRequestType class/interface). */
#define PRINTER_REQ_GET_DEVICE_ID 0x00
#define PRINTER_REQ_GET_PORT_STATUS 0x01
#define PRINTER_REQ_SOFT_RESET 0x02
#define PRINTER_STATUS_PAPER_EMPTY 0x08
#define PRINTER_STATUS_SELECTED 0x10
#define PRINTER_STATUS_NOT_ERROR 0x20

#define EVT_NEW BIT0
#define EVT_GONE BIT1
#define EVT_DATA BIT2

#define RING_BYTES (256 * 1024)
#define RING_FALLBACK_BYTES (48 * 1024)
#define XFER_BYTES (16 * 1024)
#define WRITE_WAIT_MS 30000
#define XFER_WAIT_MS 20000

static SemaphoreHandle_t s_host_ready;
static SemaphoreHandle_t s_mu;
static SemaphoreHandle_t s_space_sem;
static SemaphoreHandle_t s_xfer_sem;
static EventGroupHandle_t s_events;

static usb_host_client_handle_t s_client;
static usb_device_handle_t s_dev;
static usb_transfer_t *s_out;
static usb_transfer_t *s_in;
static usb_transfer_t *s_ctrl;
static SemaphoreHandle_t s_in_mu;
static SemaphoreHandle_t s_in_sem;
static volatile bool s_in_busy;
static uint8_t s_in_ep;

static uint8_t *s_ring;
static size_t s_ring_cap;
static size_t s_head;
static size_t s_tail;
static size_t s_used;
static bool s_ready;
static bool s_dead_write;
static uint8_t s_if_num;
static uint8_t s_out_ep;
static uint16_t s_out_mps;
static uint8_t s_new_addr;
static uint32_t s_wait_seq;
static SemaphoreHandle_t s_job_mu;
static SemaphoreHandle_t s_finish_sem;
static volatile bool s_finish_req;
static bool s_need_zlp;
static volatile bool s_xfer_busy;
static volatile uint32_t s_completed_seq;
static int s_fail_streak;

#define HOTPLUG_MAX 2

static usb_printer_hotplug_cb_t s_hotplug[HOTPLUG_MAX];
static usb_printer_identity_t s_ident;

static void notify_hotplug(bool ready)
{
    usb_printer_hotplug_cb_t cbs[HOTPLUG_MAX];
    memcpy(cbs, s_hotplug, sizeof(cbs));
    for (int i = 0; i < HOTPLUG_MAX; i++) {
        if (cbs[i]) {
            cbs[i](ready);
        }
    }
}

bool usb_printer_is_ready(void)
{
    return s_ready;
}

void usb_printer_set_hotplug_cb(usb_printer_hotplug_cb_t cb)
{
    s_hotplug[0] = cb;
}

void usb_printer_add_hotplug_cb(usb_printer_hotplug_cb_t cb)
{
    if (!cb) {
        return;
    }
    for (int i = 0; i < HOTPLUG_MAX; i++) {
        if (s_hotplug[i] == cb) {
            return;
        }
    }
    for (int i = 0; i < HOTPLUG_MAX; i++) {
        if (!s_hotplug[i]) {
            s_hotplug[i] = cb;
            return;
        }
    }
    ESP_LOGW(TAG, "hotplug listener dropped");
}

bool usb_printer_copy_identity(usb_printer_identity_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!s_mu) {
        return false;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    *out = s_ident;
    xSemaphoreGive(s_mu);
    return out->present;
}

static void xfer_cb(usb_transfer_t *transfer)
{
    /* Copy the generation before any later submit reuses this transfer. */
    s_completed_seq = (uint32_t)(uintptr_t)transfer->context;
    s_xfer_busy = false;
    xSemaphoreGive(s_xfer_sem);
}

static void in_cb(usb_transfer_t *transfer)
{
    (void)transfer;
    xSemaphoreGive(s_in_sem);
}

static esp_err_t submit_and_wait(usb_transfer_t *transfer, bool control)
{
    if (s_xfer_busy) {
        if (xSemaphoreTake(s_xfer_sem, pdMS_TO_TICKS(XFER_WAIT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "previous USB transfer still in flight");
            return ESP_ERR_INVALID_STATE;
        }
        s_xfer_busy = false;
    }
    while (xSemaphoreTake(s_xfer_sem, 0) == pdTRUE) {
    }

    s_wait_seq++;
    if (s_wait_seq == 0) {
        s_wait_seq = 1;
    }
    uint32_t seq = s_wait_seq;
    transfer->context = (void *)(uintptr_t)seq;
    transfer->callback = xfer_cb;
    transfer->device_handle = s_dev;
    s_xfer_busy = true;

    esp_err_t err = control ? usb_host_transfer_submit_control(s_client, transfer)
                            : usb_host_transfer_submit(transfer);
    if (err != ESP_OK) {
        s_xfer_busy = false;
        return err;
    }

    if (xSemaphoreTake(s_xfer_sem, pdMS_TO_TICKS(XFER_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "USB transfer timed out");
        usb_host_endpoint_halt(s_dev, transfer->bEndpointAddress);
        usb_host_endpoint_flush(s_dev, transfer->bEndpointAddress);
        if (xSemaphoreTake(s_xfer_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        s_xfer_busy = false;
        return ESP_ERR_TIMEOUT;
    }
    s_xfer_busy = false;
    if (s_completed_seq != seq) {
        ESP_LOGW(TAG, "USB completion generation %u, expected %u", (unsigned)s_completed_seq, (unsigned)seq);
        return ESP_FAIL;
    }

    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "USB transfer status %d ep 0x%02x", (int)transfer->status,
                 transfer->bEndpointAddress);
        if (transfer->status == USB_TRANSFER_STATUS_STALL) {
            usb_host_endpoint_clear(s_dev, transfer->bEndpointAddress);
        }
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t printer_control(uint8_t request, uint8_t *data, uint16_t wlen, bool data_in)
{
    if (!s_ctrl) {
        return ESP_ERR_INVALID_STATE;
    }

    usb_setup_packet_t *setup = (usb_setup_packet_t *)s_ctrl->data_buffer;
    setup->bmRequestType = data_in ? 0xA1 : 0x21;
    setup->bRequest = request;
    setup->wValue = 0;
    setup->wIndex = s_if_num;
    setup->wLength = wlen;
    s_ctrl->num_bytes = (int)sizeof(*setup) + (int)wlen;
    s_ctrl->bEndpointAddress = 0;
    s_ctrl->timeout_ms = 1000;

    esp_err_t err = submit_and_wait(s_ctrl, true);
    if (err != ESP_OK) {
        return err;
    }
    if (data_in && data && wlen > 0) {
        int got = s_ctrl->actual_num_bytes - (int)sizeof(*setup);
        if (got < 0) {
            got = 0;
        }
        if (got > (int)wlen) {
            got = (int)wlen;
        }
        memcpy(data, s_ctrl->data_buffer + sizeof(*setup), (size_t)got);
    }
    return ESP_OK;
}

static void log_port_status(void)
{
    uint8_t status = 0;
    if (printer_control(PRINTER_REQ_GET_PORT_STATUS, &status, 1, true) != ESP_OK) {
        ESP_LOGW(TAG, "GET_PORT_STATUS failed (continuing)");
        return;
    }
    ESP_LOGI(TAG, "port status 0x%02x selected=%d paper_empty=%d not_error=%d", status,
             (status & PRINTER_STATUS_SELECTED) ? 1 : 0, (status & PRINTER_STATUS_PAPER_EMPTY) ? 1 : 0,
             (status & PRINTER_STATUS_NOT_ERROR) ? 1 : 0);
}

/* The USB host ignores transfer->timeout_ms, so a missed ~HI must not use the 20s job wait. */
#define HI_WAIT_MS 1000

static void recover_endpoint(uint8_t ep, SemaphoreHandle_t done)
{
    if (!s_dev) {
        return;
    }
    usb_host_endpoint_halt(s_dev, ep);
    usb_host_endpoint_flush(s_dev, ep);
    if (done) {
        xSemaphoreTake(done, pdMS_TO_TICKS(500));
    }
    usb_host_endpoint_clear(s_dev, ep);
}

/* ~HI runs before the printer is marked ready, on the same task as the pump. */
static void query_zpl_firmware(char *fw, size_t fw_len)
{
    fw[0] = '\0';
    if (!s_out || !s_in || !s_dev) {
        return;
    }

    static const char hi[] = "~HI\r\n";
    size_t len = sizeof(hi) - 1;
    memcpy(s_out->data_buffer, hi, len);
    s_out->num_bytes = (int)len;
    s_out->bEndpointAddress = s_out_ep;
    s_out->device_handle = s_dev;
    s_out->timeout_ms = 0;
    s_out->flags = 0;
    s_wait_seq++;
    if (s_wait_seq == 0) {
        s_wait_seq = 1;
    }
    uint32_t seq = s_wait_seq;
    s_out->context = (void *)(uintptr_t)seq;
    s_out->callback = xfer_cb;
    while (xSemaphoreTake(s_xfer_sem, 0) == pdTRUE) {
    }
    s_xfer_busy = true;
    if (usb_host_transfer_submit(s_out) != ESP_OK) {
        s_xfer_busy = false;
        ESP_LOGW(TAG, "~HI was not accepted");
        return;
    }
    if (xSemaphoreTake(s_xfer_sem, pdMS_TO_TICKS(HI_WAIT_MS)) != pdTRUE) {
        recover_endpoint(s_out_ep, s_xfer_sem);
        s_xfer_busy = false;
        ESP_LOGW(TAG, "~HI OUT timed out");
        return;
    }
    s_xfer_busy = false;
    if (s_completed_seq != seq || s_out->status != USB_TRANSFER_STATUS_COMPLETED) {
        if (s_out->status == USB_TRANSFER_STATUS_STALL) {
            usb_host_endpoint_clear(s_dev, s_out_ep);
        }
        ESP_LOGW(TAG, "~HI was not accepted");
        return;
    }

    int ask = 512;
    if (ask > s_in->data_buffer_size) {
        ask = s_in->data_buffer_size;
    }
    s_in->num_bytes = ask;
    s_in->bEndpointAddress = s_in_ep;
    s_in->device_handle = s_dev;
    s_in->timeout_ms = 0;
    s_in->flags = 0;
    s_in->callback = in_cb;
    s_in->context = NULL;
    while (xSemaphoreTake(s_in_sem, 0) == pdTRUE) {
    }
    s_in_busy = true;
    if (usb_host_transfer_submit(s_in) != ESP_OK) {
        s_in_busy = false;
        ESP_LOGI(TAG, "~HI reply unavailable");
        return;
    }
    if (xSemaphoreTake(s_in_sem, pdMS_TO_TICKS(HI_WAIT_MS)) != pdTRUE) {
        recover_endpoint(s_in_ep, s_in_sem);
        s_in_busy = false;
        ESP_LOGI(TAG, "~HI reply unavailable");
        return;
    }
    s_in_busy = false;
    if (s_in->status == USB_TRANSFER_STATUS_STALL) {
        usb_host_endpoint_clear(s_dev, s_in_ep);
        return;
    }
    int n = s_in->actual_num_bytes;
    if (n <= 0) {
        return;
    }
    if (n > s_in->data_buffer_size) {
        n = s_in->data_buffer_size;
    }
    printer_id_take_firmware(s_in->data_buffer, n, fw, fw_len);
    if (fw[0]) {
        ESP_LOGI(TAG, "printer firmware %s", fw);
    }
}

static void read_identity(usb_printer_identity_t *id)
{
    memset(id, 0, sizeof(*id));
    uint8_t raw[256];
    memset(raw, 0, sizeof(raw));
    if (printer_control(PRINTER_REQ_GET_DEVICE_ID, raw, sizeof(raw) - 1, true) != ESP_OK) {
        ESP_LOGI(TAG, "GET_DEVICE_ID not available");
        return;
    }
    int got = s_ctrl->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
    if (got < 2) {
        return;
    }
    /* First two bytes are a big-endian length that includes itself. */
    int n = ((int)raw[0] << 8) | raw[1];
    int slen = n - 2;
    if (slen < 0) {
        return;
    }
    if (slen > got - 2) {
        slen = got - 2;
    }
    if (slen >= (int)sizeof(id->device_id)) {
        slen = (int)sizeof(id->device_id) - 1;
    }
    memcpy(id->device_id, raw + 2, (size_t)slen);
    id->device_id[slen] = '\0';
    printer_id_parse(id);
    if (!id->device_id[0]) {
        return;
    }
    ESP_LOGI(TAG, "device id: %s", id->device_id);
    if (id->zpl) {
        query_zpl_firmware(id->firmware, sizeof(id->firmware));
    }
}

static bool find_printer(const usb_config_desc_t *cfg, const usb_intf_desc_t **intf_out,
                         const usb_ep_desc_t **out_ep, const usb_ep_desc_t **in_ep)
{
    int offset = 0;
    const usb_standard_desc_t *cur = (const usb_standard_desc_t *)cfg;
    *intf_out = NULL;
    *out_ep = NULL;
    *in_ep = NULL;

    while ((cur = usb_parse_next_descriptor_of_type(cur, cfg->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE,
                                                    &offset)) != NULL) {
        const usb_intf_desc_t *intf = (const usb_intf_desc_t *)cur;
        if (intf->bInterfaceClass != USB_CLASS_PRINTER || intf->bAlternateSetting != 0) {
            continue;
        }
        if (intf->bInterfaceSubClass != 0x01 && intf->bInterfaceSubClass != 0x00) {
            continue;
        }

        const usb_ep_desc_t *bulk_out = NULL;
        const usb_ep_desc_t *bulk_in = NULL;
        for (int i = 0; i < intf->bNumEndpoints; i++) {
            int ep_off = offset;
            const usb_ep_desc_t *ep =
                usb_parse_endpoint_descriptor_by_index(intf, i, cfg->wTotalLength, &ep_off);
            if (!ep) {
                continue;
            }
            if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) != USB_BM_ATTRIBUTES_XFER_BULK) {
                continue;
            }
            if (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) {
                bulk_in = ep;
            } else {
                bulk_out = ep;
            }
        }
        if (!bulk_out) {
            continue;
        }
        *intf_out = intf;
        *out_ep = bulk_out;
        *in_ep = bulk_in;
        return true;
    }
    return false;
}

static void release_finish_locked(void)
{
    s_finish_req = false;
    s_need_zlp = false;
}

static void discard_ring(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_head = 0;
    s_tail = 0;
    s_used = 0;
    s_dead_write = true;
    release_finish_locked();
    xSemaphoreGive(s_mu);
    xSemaphoreGive(s_space_sem);
    xSemaphoreGive(s_finish_sem);
}

static void detach_printer(void)
{
    if (s_mu) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        s_ready = false;
        memset(&s_ident, 0, sizeof(s_ident));
        xSemaphoreGive(s_mu);
    } else {
        s_ready = false;
        memset(&s_ident, 0, sizeof(s_ident));
    }
    notify_hotplug(false);
    discard_ring();

    if (s_dev) {
        if (s_out) {
            usb_host_endpoint_halt(s_dev, s_out_ep);
            usb_host_endpoint_flush(s_dev, s_out_ep);
        }
        if (s_in && s_in_busy) {
            usb_host_endpoint_halt(s_dev, s_in_ep);
            usb_host_endpoint_flush(s_dev, s_in_ep);
        }
        if (s_xfer_busy) {
            xSemaphoreTake(s_xfer_sem, pdMS_TO_TICKS(1000));
            s_xfer_busy = false;
        }
        TickType_t wait_in = xTaskGetTickCount();
        while (s_in_busy && (xTaskGetTickCount() - wait_in) < pdMS_TO_TICKS(1000)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        usb_host_interface_release(s_client, s_dev, s_if_num);
        usb_host_device_close(s_client, s_dev);
        s_dev = NULL;
    }
    if (s_in_mu) {
        xSemaphoreTake(s_in_mu, portMAX_DELAY);
    }
    if (s_out) {
        usb_host_transfer_free(s_out);
        s_out = NULL;
    }
    if (s_in) {
        usb_host_transfer_free(s_in);
        s_in = NULL;
    }
    s_in_busy = false;
    if (s_in_mu) {
        xSemaphoreGive(s_in_mu);
    }
    if (s_ctrl) {
        usb_host_transfer_free(s_ctrl);
        s_ctrl = NULL;
    }
    ESP_LOGI(TAG, "printer detached");
}

static void attach_printer(uint8_t addr)
{
    if (s_dev) {
        ESP_LOGW(TAG, "ignoring extra device addr %u while a printer is open", addr);
        return;
    }

    usb_device_handle_t dev = NULL;
    esp_err_t err = usb_host_device_open(s_client, addr, &dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device open addr %u: %s", addr, esp_err_to_name(err));
        return;
    }

    const usb_device_desc_t *dd = NULL;
    usb_host_get_device_descriptor(dev, &dd);
    if (dd) {
        ESP_LOGI(TAG, "USB device %04x:%04x class 0x%02x", dd->idVendor, dd->idProduct, dd->bDeviceClass);
    }

    const usb_config_desc_t *cfg = NULL;
    err = usb_host_get_active_config_descriptor(dev, &cfg);
    if (err != ESP_OK || !cfg) {
        ESP_LOGE(TAG, "config descriptor: %s", esp_err_to_name(err));
        usb_host_device_close(s_client, dev);
        return;
    }

    const usb_intf_desc_t *intf = NULL;
    const usb_ep_desc_t *out_ep = NULL;
    const usb_ep_desc_t *in_ep = NULL;
    if (!find_printer(cfg, &intf, &out_ep, &in_ep)) {
        ESP_LOGI(TAG, "device addr %u is not printer class 0x07; closing", addr);
        usb_host_device_close(s_client, dev);
        return;
    }

    err = usb_host_interface_claim(s_client, dev, intf->bInterfaceNumber, intf->bAlternateSetting);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "claim if %u: %s", intf->bInterfaceNumber, esp_err_to_name(err));
        usb_host_device_close(s_client, dev);
        return;
    }

    s_dev = dev;
    s_if_num = intf->bInterfaceNumber;
    s_out_ep = out_ep->bEndpointAddress;
    s_out_mps = out_ep->wMaxPacketSize & USB_W_MAX_PACKET_SIZE_MPS_MASK;
    if (s_out_mps == 0) {
        s_out_mps = 64;
    }

    size_t xfer_bytes = XFER_BYTES;
    err = usb_host_transfer_alloc(xfer_bytes, 0, &s_out);
    if (err != ESP_OK) {
        xfer_bytes = 4096;
        err = usb_host_transfer_alloc(xfer_bytes, 0, &s_out);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OUT transfer alloc failed");
        usb_host_interface_release(s_client, dev, s_if_num);
        usb_host_device_close(s_client, dev);
        s_dev = NULL;
        return;
    }
    s_out->bEndpointAddress = s_out_ep;
    s_out->device_handle = dev;

    if (usb_host_transfer_alloc(512, 0, &s_ctrl) != ESP_OK) {
        ESP_LOGW(TAG, "control transfer alloc failed; status requests skipped");
        s_ctrl = NULL;
    }

    s_in = NULL;
    s_in_ep = 0;
    if (in_ep) {
        s_in_ep = in_ep->bEndpointAddress;
        if (usb_host_transfer_alloc(2048, 0, &s_in) != ESP_OK) {
            ESP_LOGW(TAG, "IN transfer alloc failed; printer replies will be dropped");
            s_in = NULL;
        } else {
            s_in->bEndpointAddress = s_in_ep;
            s_in->device_handle = dev;
        }
    }

    ESP_LOGI(TAG, "claimed printer if %u protocol %u bulk OUT ep 0x%02x mps %u%s", intf->bInterfaceNumber,
             intf->bInterfaceProtocol, s_out_ep, s_out_mps, s_in ? " (bulk IN pass-through)" : "");

    printer_control(PRINTER_REQ_SOFT_RESET, NULL, 0, false);
    vTaskDelay(pdMS_TO_TICKS(200));
    usb_printer_identity_t ident;
    read_identity(&ident);
    if (s_events && (xEventGroupGetBits(s_events) & EVT_GONE)) {
        ESP_LOGW(TAG, "printer left during identification");
        return;
    }
    log_port_status();

    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_ident = ident;
    s_dead_write = false;
    s_head = s_tail = s_used = 0;
    s_ready = true;
    xSemaphoreGive(s_mu);

    notify_hotplug(true);
}

static void ring_copy_out(uint8_t *dst, size_t n)
{
    size_t tail = s_tail;
    size_t first = s_ring_cap - tail;
    if (first > n) {
        first = n;
    }
    memcpy(dst, s_ring + tail, first);
    if (n > first) {
        memcpy(dst + first, s_ring, n - first);
    }
}

static void ring_consume(size_t n)
{
    s_tail += n;
    if (s_tail >= s_ring_cap) {
        s_tail -= s_ring_cap;
    }
    s_used -= n;
}

static void drop_failed_job(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_head = 0;
    s_tail = 0;
    s_used = 0;
    release_finish_locked();
    s_fail_streak = 0;
    xSemaphoreGive(s_mu);
    xSemaphoreGive(s_space_sem);
    xSemaphoreGive(s_finish_sem);
    ESP_LOGE(TAG, "dropped the USB job after repeated OUT failures");
}

static esp_err_t send_trailing_zlp(void)
{
    s_out->num_bytes = 0;
    s_out->bEndpointAddress = s_out_ep;
    s_out->device_handle = s_dev;
    /* One descriptor of length 0 is the trailing short packet. */
    s_out->flags = 0;
    esp_err_t err = submit_and_wait(s_out, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "trailing zero-length packet: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t pump_once(void)
{
    if (!s_ready || !s_out) {
        if (s_finish_req) {
            xSemaphoreGive(s_finish_sem);
        }
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mu, portMAX_DELAY);
    size_t n = s_used;
    if (n > s_out->data_buffer_size) {
        n = s_out->data_buffer_size;
    }
    bool ending = s_finish_req && n == s_used;
    bool need_zlp = s_need_zlp;
    if (n > 0) {
        ring_copy_out(s_out->data_buffer, n);
    }
    xSemaphoreGive(s_mu);

    if (n == 0) {
        esp_err_t err = ESP_OK;
        if (ending && need_zlp) {
            err = send_trailing_zlp();
        }
        if (ending) {
            xSemaphoreTake(s_mu, portMAX_DELAY);
            release_finish_locked();
            xSemaphoreGive(s_mu);
            xSemaphoreGive(s_finish_sem);
        }
        return err;
    }

    s_out->num_bytes = (int)n;
    s_out->bEndpointAddress = s_out_ep;
    s_out->device_handle = s_dev;
    s_out->flags = 0;
    if (ending && s_out_mps != 0 && (n % s_out_mps) == 0) {
        s_out->flags = USB_TRANSFER_FLAG_ZERO_PACK;
    }

    esp_err_t err = submit_and_wait(s_out, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bulk OUT %u bytes failed: %s", (unsigned)n, esp_err_to_name(err));
        s_fail_streak++;
        if (s_fail_streak >= 3) {
            drop_failed_job();
        }
        return err;
    }

    xSemaphoreTake(s_mu, portMAX_DELAY);
    ring_consume(n);
    s_fail_streak = 0;
    s_need_zlp = (s_out->flags & USB_TRANSFER_FLAG_ZERO_PACK) == 0 && s_out_mps != 0 && (n % s_out_mps) == 0;
    if (ending) {
        release_finish_locked();
    }
    xSemaphoreGive(s_mu);
    xSemaphoreGive(s_space_sem);
    if (ending) {
        xSemaphoreGive(s_finish_sem);
    }
    return ESP_OK;
}

static void printer_task(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_host_ready, portMAX_DELAY);
    xSemaphoreGive(s_host_ready);

    while (true) {
        EventBits_t bits = xEventGroupWaitBits(s_events, EVT_NEW | EVT_GONE | EVT_DATA, pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & EVT_GONE) {
            detach_printer();
        }
        if (bits & EVT_NEW) {
            uint8_t addr = s_new_addr;
            attach_printer(addr);
        }
        if (bits & EVT_DATA) {
            if (!s_ready) {
                if (s_finish_req) {
                    xSemaphoreGive(s_finish_sem);
                }
            } else if (pump_once() == ESP_OK) {
                bool more = false;
                if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) == pdTRUE) {
                    more = s_used > 0 || s_finish_req;
                    xSemaphoreGive(s_mu);
                }
                if (more) {
                    xEventGroupSetBits(s_events, EVT_DATA);
                }
            } else if (s_fail_streak > 0) {
                vTaskDelay(pdMS_TO_TICKS(20));
                xEventGroupSetBits(s_events, EVT_DATA);
            }
        }
    }
}

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    (void)arg;
    if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        s_new_addr = event_msg->new_dev.address;
        xEventGroupSetBits(s_events, EVT_NEW);
    } else if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        xEventGroupSetBits(s_events, EVT_GONE);
    }
}

static void client_task(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_host_ready, portMAX_DELAY);
    xSemaphoreGive(s_host_ready);

    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };

    esp_err_t err = usb_host_client_register(&client_config, &s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "client register: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
    }
}

static void host_lib_task(void *arg)
{
    (void)arg;

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        /* Bit 0 is the high-speed OTG controller on the USB-A port. */
        .peripheral_map = BIT0,
    };

    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    xSemaphoreGive(s_host_ready);
    ESP_LOGI(TAG, "USB host installed on HS peripheral; waiting for class 0x07");

    while (true) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "all USB devices freed");
        }
    }
}

esp_err_t usb_printer_flush(uint32_t timeout_ms)
{
    if (!s_mu) {
        return ESP_ERR_INVALID_STATE;
    }
    TickType_t start = xTaskGetTickCount();
    while (true) {
        bool idle = false;
        if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(200)) == pdTRUE) {
            idle = s_ready && !s_dead_write && s_used == 0 && !s_xfer_busy;
            bool dead = !s_ready || s_dead_write;
            xSemaphoreGive(s_mu);
            if (dead) {
                return ESP_ERR_INVALID_STATE;
            }
        }
        if (idle) {
            return ESP_OK;
        }
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms)) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

int usb_printer_read(uint8_t *dst, size_t dst_len, uint32_t timeout_ms)
{
    if (!dst || dst_len == 0 || !s_in_mu) {
        return -1;
    }
    if (timeout_ms < 50) {
        timeout_ms = 50;
    }

    xSemaphoreTake(s_in_mu, portMAX_DELAY);
    if (!s_ready || !s_in || !s_dev || s_in_busy) {
        bool have = s_in != NULL;
        xSemaphoreGive(s_in_mu);
        return have ? 0 : -1;
    }
    while (xSemaphoreTake(s_in_sem, 0) == pdTRUE) {
    }
    size_t ask = dst_len;
    if (ask > (size_t)s_in->data_buffer_size) {
        ask = (size_t)s_in->data_buffer_size;
    }
    s_in->num_bytes = (int)ask;
    s_in->timeout_ms = timeout_ms;
    s_in->bEndpointAddress = s_in_ep;
    s_in->device_handle = s_dev;
    s_in->callback = in_cb;
    s_in->context = NULL;
    s_in->flags = 0;
    s_in_busy = true;
    esp_err_t err = usb_host_transfer_submit(s_in);
    xSemaphoreGive(s_in_mu);
    if (err != ESP_OK) {
        s_in_busy = false;
        ESP_LOGW(TAG, "bulk IN submit: %s", esp_err_to_name(err));
        return -1;
    }

    if (xSemaphoreTake(s_in_sem, pdMS_TO_TICKS(timeout_ms + 500)) != pdTRUE) {
        if (s_dev) {
            usb_host_endpoint_halt(s_dev, s_in_ep);
            usb_host_endpoint_flush(s_dev, s_in_ep);
        }
        xSemaphoreTake(s_in_sem, pdMS_TO_TICKS(500));
        s_in_busy = false;
        return 0;
    }

    xSemaphoreTake(s_in_mu, portMAX_DELAY);
    int n = 0;
    if (s_in && s_in->actual_num_bytes > 0 &&
        (s_in->status == USB_TRANSFER_STATUS_COMPLETED || s_in->status == USB_TRANSFER_STATUS_TIMED_OUT)) {
        n = s_in->actual_num_bytes;
        if ((size_t)n > dst_len) {
            n = (int)dst_len;
        }
        memcpy(dst, s_in->data_buffer, (size_t)n);
    } else if (s_in && s_in->status == USB_TRANSFER_STATUS_STALL) {
        if (s_dev) {
            usb_host_endpoint_clear(s_dev, s_in_ep);
        }
        n = -1;
    }
    s_in_busy = false;
    xSemaphoreGive(s_in_mu);
    return n;
}

esp_err_t usb_printer_write(const uint8_t *data, size_t len)
{
    if (data == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }
    if (!s_ring || !s_mu) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t off = 0;
    TickType_t start = xTaskGetTickCount();
    while (off < len) {
        if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(1000)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        if (!s_ready || s_dead_write) {
            xSemaphoreGive(s_mu);
            return ESP_ERR_INVALID_STATE;
        }
        size_t space = s_ring_cap - s_used;
        if (space == 0) {
            xSemaphoreGive(s_mu);
            if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(WRITE_WAIT_MS)) {
                return ESP_ERR_TIMEOUT;
            }
            xSemaphoreTake(s_space_sem, pdMS_TO_TICKS(200));
            continue;
        }

        size_t n = len - off;
        if (n > space) {
            n = space;
        }
        size_t first = s_ring_cap - s_head;
        if (first > n) {
            first = n;
        }
        memcpy(s_ring + s_head, data + off, first);
        if (n > first) {
            memcpy(s_ring, data + off + first, n - first);
        }
        s_head += n;
        if (s_head >= s_ring_cap) {
            s_head -= s_ring_cap;
        }
        s_used += n;
        off += n;
        xSemaphoreGive(s_mu);
        xEventGroupSetBits(s_events, EVT_DATA);
    }
    return ESP_OK;
}

esp_err_t usb_printer_job_begin(void)
{
    if (!s_job_mu) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_job_mu, pdMS_TO_TICKS(60000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t usb_printer_job_end(void)
{
    if (!s_job_mu || !s_mu) {
        return ESP_ERR_INVALID_STATE;
    }
    while (xSemaphoreTake(s_finish_sem, 0) == pdTRUE) {
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(1000)) != pdTRUE) {
        xSemaphoreGive(s_job_mu);
        return ESP_ERR_TIMEOUT;
    }
    bool ready = s_ready && !s_dead_write;
    if (ready) {
        s_finish_req = true;
    }
    xSemaphoreGive(s_mu);
    if (!ready) {
        xSemaphoreGive(s_job_mu);
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupSetBits(s_events, EVT_DATA);
    esp_err_t err = ESP_OK;
    if (xSemaphoreTake(s_finish_sem, pdMS_TO_TICKS(WRITE_WAIT_MS)) != pdTRUE) {
        err = ESP_ERR_TIMEOUT;
    }
    xSemaphoreGive(s_job_mu);
    return err;
}

esp_err_t usb_printer_start(void)
{
    s_ring = heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_ring_cap = RING_BYTES;
    if (!s_ring) {
        s_ring = heap_caps_malloc(RING_FALLBACK_BYTES, MALLOC_CAP_8BIT);
        s_ring_cap = s_ring ? RING_FALLBACK_BYTES : 0;
    }
    if (!s_ring) {
        return ESP_ERR_NO_MEM;
    }

    s_host_ready = xSemaphoreCreateBinary();
    s_mu = xSemaphoreCreateMutex();
    s_job_mu = xSemaphoreCreateMutex();
    s_space_sem = xSemaphoreCreateBinary();
    s_xfer_sem = xSemaphoreCreateBinary();
    s_finish_sem = xSemaphoreCreateBinary();
    s_in_mu = xSemaphoreCreateMutex();
    s_in_sem = xSemaphoreCreateBinary();
    s_events = xEventGroupCreate();
    if (!s_host_ready || !s_mu || !s_job_mu || !s_space_sem || !s_xfer_sem || !s_finish_sem || !s_in_mu || !s_in_sem ||
        !s_events) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(host_lib_task, "usb_host", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(client_task, "usb_client", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(printer_task, "usb_prn", 6144, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "USB printer host started, ring %u bytes", (unsigned)s_ring_cap);
    return ESP_OK;
}
