/* Shared USB Host core: usb_host_lib installation, asynchronous client,
 * dispatch of devices to the drivers (MIDI / Rescue).
 */
#include "drydock_usb_host.h"

#include <stdarg.h>
#include <string.h>

#include "esp_check.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dry_usb_host";

static const drydock_usb_driver_t *s_drivers;
static size_t s_n_drivers;
static usb_host_client_handle_t s_client;
static usb_device_handle_t s_dev;
static const drydock_usb_driver_t *s_active;
static TaskHandle_t s_task;

/* ---- journal (visible on the USB Diag screen when the console is dead) ---- */

static char s_journal[DRYDOCK_USB_JOURNAL_LINES][72];
static int s_journal_len;

void drydock_usb_journal_add(const char *fmt, ...)
{
    char line[72];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    /* client task and probe only: no lock needed around the memmove */
    memmove(s_journal[0], s_journal[1], (DRYDOCK_USB_JOURNAL_LINES - 1) * sizeof s_journal[0]);
    snprintf(s_journal[DRYDOCK_USB_JOURNAL_LINES - 1], sizeof s_journal[0], "%s", line);
    if (s_journal_len < DRYDOCK_USB_JOURNAL_LINES) {
        s_journal_len++;
    }
}

static void jlog(const char *fmt, ...)
{
    char line[72];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    drydock_usb_journal_add("%s", line);
}

const char *drydock_usb_journal(int *count)
{
    if (count != NULL) {
        *count = s_journal_len;
    }
    return &s_journal[0][0];
}

usb_host_client_handle_t drydock_usb_host_client(void)
{
    return s_client;
}

static void on_new_dev(uint8_t addr)
{
    usb_device_handle_t dev = NULL;
    if (usb_host_device_open(s_client, addr, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "device_open(addr %u) failed", addr);
        return;
    }
    const usb_device_desc_t *ddesc = NULL;
    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_device_descriptor(dev, &ddesc) != ESP_OK || ddesc == NULL ||
        usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK || cfg == NULL) {
        ESP_LOGW(TAG, "descriptors unavailable addr %u", addr);
        usb_host_device_close(s_client, dev);
        return;
    }
    ESP_LOGI(TAG, "device addr %u: VID=%04X PID=%04X class=0x%02X",
             addr, ddesc->idVendor, ddesc->idProduct, ddesc->bDeviceClass);
    jlog("dev addr=%u VID=%04X PID=%04X cls=%02X",
         addr, ddesc->idVendor, ddesc->idProduct, ddesc->bDeviceClass);

    for (size_t i = 0; i < s_n_drivers; i++) {
        if (!s_drivers[i].match(ddesc, cfg)) {
            continue;
        }
        const esp_err_t ret = s_drivers[i].attach(s_client, dev, cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "attach %s: %s", s_drivers[i].name, esp_err_to_name(ret));
            jlog("attach %s FAIL %s", s_drivers[i].name, esp_err_to_name(ret));
            continue; /* this driver refused: try the next one */
        }
        s_dev = dev;
        s_active = &s_drivers[i];
        ESP_LOGI(TAG, "active driver: %s", s_drivers[i].name);
        jlog("attached %s", s_drivers[i].name);
        return;
    }
    ESP_LOGW(TAG, "no driver for addr %u, closing", addr);
    jlog("NO DRIVER for VID=%04X PID=%04X", ddesc->idVendor, ddesc->idProduct);
    usb_host_device_close(s_client, dev);
}

static void on_dev_gone(void)
{
    if (s_active != NULL) {
        ESP_LOGI(TAG, "device gone, detach %s", s_active->name);
        jlog("gone: detach %s", s_active->name);
        s_active->detach();
        s_active = NULL;
    }
    if (s_dev != NULL) {
        usb_host_device_close(s_client, s_dev);
        s_dev = NULL;
    }
}

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        on_new_dev(msg->new_dev.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        on_dev_gone();
        break;
    default:
        break;
    }
}

static void client_task(void *arg)
{
    (void)arg;
    const usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    if (usb_host_client_register(&cfg, &s_client) != ESP_OK) {
        ESP_LOGE(TAG, "client_register failed");
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "USB Host client ready");
    jlog("usb host client ready");
    while (1) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
    }
}

esp_err_t drydock_usb_host_start(const drydock_usb_driver_t *drivers, size_t n_drivers)
{
    if (drivers == NULL || n_drivers == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    const esp_err_t inst = usb_host_install(&host_cfg);
    if (inst != ESP_OK) {
        jlog("usb_host_install FAIL %s", esp_err_to_name(inst));
        return inst;
    }
    jlog("usb_host_install ok");

    s_drivers = drivers;
    s_n_drivers = n_drivers;
    const BaseType_t ok = xTaskCreate(client_task, "usb_host", 4096, NULL, 5, &s_task);
    if (ok != pdPASS) {
        usb_host_uninstall();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t drydock_usb_host_stop(void)
{
    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    on_dev_gone();
    if (s_client != NULL) {
        usb_host_client_deregister(s_client);
        s_client = NULL;
    }
    return usb_host_uninstall();
}

const usb_intf_desc_t *drydock_usb_find_interface(const usb_config_desc_t *cfg,
                                                  int class_, int subclass, int proto)
{
    if (cfg == NULL) {
        return NULL;
    }
    const uint8_t *p = (const uint8_t *)cfg;
    const uint8_t *end = p + cfg->wTotalLength;
    p += USB_CONFIG_DESC_SIZE;
    while (p + USB_INTF_DESC_SIZE <= end) {
        const usb_intf_desc_t *iface = (const usb_intf_desc_t *)p;
        if (iface->bDescriptorType != USB_W_VALUE_DT_INTERFACE) {
            p++;
            continue;
        }
        const int c_ok = (class_ < 0 || iface->bInterfaceClass == class_);
        const int s_ok = (subclass < 0 || iface->bInterfaceSubClass == subclass);
        const int p_ok = (proto < 0 || iface->bInterfaceProtocol == proto);
        if (c_ok && s_ok && p_ok) {
            return iface;
        }
        /* skip the interface and its endpoints (until the next interface) */
        p += iface->bLength;
        while (p + USB_INTF_DESC_SIZE <= end &&
               ((const usb_intf_desc_t *)p)->bDescriptorType != USB_W_VALUE_DT_INTERFACE) {
            p += ((const uint8_t *)p)[0] ? ((const uint8_t *)p)[0] : 1;
        }
    }
    return NULL;
}

const usb_intf_desc_t *drydock_usb_find_data_iface(const usb_config_desc_t *cfg,
                                                   int class_, int subclass, int proto)
{
    if (cfg == NULL) {
        return NULL;
    }
    const uint8_t *p = (const uint8_t *)cfg;
    const uint8_t *end = p + cfg->wTotalLength;
    p += USB_CONFIG_DESC_SIZE;
    while (p + USB_INTF_DESC_SIZE <= end) {
        const usb_intf_desc_t *iface = (const usb_intf_desc_t *)p;
        if (iface->bDescriptorType != USB_W_VALUE_DT_INTERFACE) {
            p++;
            continue;
        }
        const int c_ok = (class_ < 0 || iface->bInterfaceClass == class_);
        const int s_ok = (subclass < 0 || iface->bInterfaceSubClass == subclass);
        const int p_ok = (proto < 0 || iface->bInterfaceProtocol == proto);
        if (c_ok && s_ok && p_ok &&
            drydock_usb_find_ep(iface, 0) != NULL && drydock_usb_find_ep(iface, 1) != NULL) {
            return iface;
        }
        p += iface->bLength;
        while (p + USB_INTF_DESC_SIZE <= end &&
               ((const usb_intf_desc_t *)p)->bDescriptorType != USB_W_VALUE_DT_INTERFACE) {
            p += ((const uint8_t *)p)[0] ? ((const uint8_t *)p)[0] : 1;
        }
    }
    return NULL;
}

const usb_ep_desc_t *drydock_usb_find_ep(const usb_intf_desc_t *iface, int dir_out)
{
    if (iface == NULL) {
        return NULL;
    }
    /* the endpoints follow the interface in the same buffer: walk from
     * the interface to the next one (identified by its type). */
    const uint8_t *p = (const uint8_t *)iface + iface->bLength;
    const uint8_t *end = p + 256; /* large bound: stops at the next interface */
    while (p + USB_EP_DESC_SIZE <= end) {
        const uint8_t type = ((const uint8_t *)p)[1];
        if (type == USB_W_VALUE_DT_INTERFACE) {
            break; /* next interface: we've seen everything */
        }
        if (type == USB_W_VALUE_DT_ENDPOINT) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)p;
            const int is_out = (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) == 0;
            if (is_out == dir_out) {
                return ep;
            }
        }
        const size_t len = ((const uint8_t *)p)[0];
        if (len == 0) {
            break;
        }
        p += len;
    }
    return NULL;
}
