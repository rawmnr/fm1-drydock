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
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "dry_usb_host";

enum {
    STOP_REQUESTED = BIT0,
    CLIENT_DONE = BIT1,
    DAEMON_DONE = BIT2,
};

static const drydock_usb_driver_t *const *s_drivers;
static size_t s_n_drivers;
static usb_host_client_handle_t s_client;
static usb_device_handle_t s_dev;
static const drydock_usb_driver_t *s_active;
static drydock_usb_claim_t s_claim;
static bool s_dev_gone;
static bool s_interface_claimed;
static StaticSemaphore_t s_io_storage;
static SemaphoreHandle_t s_io;
static StaticEventGroup_t s_lifecycle_storage;
static EventGroupHandle_t s_lifecycle;
static bool s_has_client_task;
static bool s_has_daemon;
static esp_err_t s_client_result;
static esp_err_t s_daemon_result;

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

esp_err_t drydock_usb_host_submit(const drydock_usb_driver_t *driver, usb_transfer_t *transfer,
                                  bool dir_out)
{
    xSemaphoreTake(s_io, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_active == driver && !(xEventGroupGetBits(s_lifecycle) & STOP_REQUESTED)) {
        transfer->device_handle = s_dev;
        transfer->bEndpointAddress = dir_out ? s_claim.ep_out : s_claim.ep_in;
        err = usb_host_transfer_submit(transfer);
    }
    xSemaphoreGive(s_io);
    return err;
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
        const drydock_usb_driver_t *driver = s_drivers[i];
        if (!driver->match(ddesc, cfg)) {
            continue;
        }
        xSemaphoreTake(s_io, portMAX_DELAY);
        const esp_err_t ret = driver->attach(s_client, dev, cfg, &s_claim);
        if (ret == ESP_OK) {
            s_dev = dev;
            s_active = driver;
            s_interface_claimed = true;
        }
        xSemaphoreGive(s_io);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "attach %s: %s", driver->name, esp_err_to_name(ret));
            jlog("attach %s FAIL %s", driver->name, esp_err_to_name(ret));
            continue; /* this driver refused: try the next one */
        }
        s_dev_gone = false;
        ESP_LOGI(TAG, "active driver: %s", driver->name);
        jlog("attached %s", driver->name);
        return;
    }
    ESP_LOGW(TAG, "no driver for addr %u, closing", addr);
    jlog("NO DRIVER for VID=%04X PID=%04X", ddesc->idVendor, ddesc->idProduct);
    usb_host_device_close(s_client, dev);
}

static esp_err_t close_device(bool cancel_transfers)
{
    if (s_dev == NULL) {
        return ESP_OK;
    }
    xSemaphoreTake(s_io, portMAX_DELAY);
    const drydock_usb_driver_t *driver = s_active;
    s_active = NULL;
    if (driver != NULL) {
        driver->detach();
    }
    xSemaphoreGive(s_io);

    if (driver != NULL) {
        ESP_LOGI(TAG, "detach %s", driver->name);
        jlog("detach %s", driver->name);
    }
    if (s_interface_claimed) {
        if (cancel_transfers) {
            ESP_RETURN_ON_ERROR(usb_host_endpoint_halt(s_dev, s_claim.ep_in), TAG, "halt IN");
            ESP_RETURN_ON_ERROR(usb_host_endpoint_halt(s_dev, s_claim.ep_out), TAG, "halt OUT");
            ESP_RETURN_ON_ERROR(usb_host_endpoint_flush(s_dev, s_claim.ep_in), TAG, "flush IN");
            ESP_RETURN_ON_ERROR(usb_host_endpoint_flush(s_dev, s_claim.ep_out), TAG, "flush OUT");
        }
        ESP_RETURN_ON_ERROR(usb_host_client_handle_events(s_client, 0), TAG, "drain transfers");
        ESP_RETURN_ON_ERROR(usb_host_interface_release(s_client, s_dev, s_claim.interface_number),
                            TAG, "release interface");
        s_interface_claimed = false;
    }
    ESP_RETURN_ON_ERROR(usb_host_device_close(s_client, s_dev), TAG, "close device");
    s_dev = NULL;
    s_dev_gone = false;
    return ESP_OK;
}

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        if (s_dev == NULL && !(xEventGroupGetBits(s_lifecycle) & STOP_REQUESTED)) {
            on_new_dev(msg->new_dev.address);
        }
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        if (msg->dev_gone.dev_hdl == s_dev) {
            s_dev_gone = true;
        }
        break;
    default:
        break;
    }
}

static void client_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "USB Host client ready");
    jlog("usb host client ready");
    while (!(xEventGroupGetBits(s_lifecycle) & STOP_REQUESTED)) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
        if (s_dev_gone) {
            close_device(false);
        }
    }
    s_client_result = close_device(!s_dev_gone);
    xEventGroupSetBits(s_lifecycle, CLIENT_DONE);
    vTaskDelete(NULL);
}

static void lib_daemon_task(void *arg)
{
    (void)arg;
    bool waiting_for_free = false;
    for (;;) {
        uint32_t flags = 0;
        s_daemon_result = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (s_daemon_result != ESP_OK) {
            break;
        }
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            const esp_err_t err = usb_host_device_free_all();
            if (err != ESP_ERR_NOT_FINISHED) {
                s_daemon_result = err;
                break;
            }
            waiting_for_free = true;
        } else if (waiting_for_free && (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)) {
            break;
        }
    }
    xEventGroupSetBits(s_lifecycle, DAEMON_DONE);
    vTaskDelete(NULL);
}

esp_err_t drydock_usb_host_start(const drydock_usb_driver_t *const *drivers, size_t n_drivers)
{
    if (drivers == NULL || n_drivers == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_client != NULL || s_has_daemon) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lifecycle == NULL) {
        s_lifecycle = xEventGroupCreateStatic(&s_lifecycle_storage);
        s_io = xSemaphoreCreateMutexStatic(&s_io_storage);
    }
    xEventGroupClearBits(s_lifecycle, STOP_REQUESTED | CLIENT_DONE | DAEMON_DONE);
    s_client_result = ESP_OK;
    s_daemon_result = ESP_OK;
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .root_port_unpowered = true,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    const esp_err_t inst = usb_host_install(&host_cfg);
    if (inst != ESP_OK) {
        jlog("usb_host_install FAIL %s", esp_err_to_name(inst));
        return inst;
    }
    jlog("usb_host_install ok");
    const usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
        },
    };
    const esp_err_t reg = usb_host_client_register(&cfg, &s_client);
    if (reg != ESP_OK) {
        usb_host_uninstall();
        return reg;
    }

    s_drivers = drivers;
    s_n_drivers = n_drivers;
    esp_err_t err = ESP_ERR_NO_MEM;
    if (xTaskCreate(client_task, "usb_host", 4096, NULL, 5, NULL) == pdPASS) {
        s_has_client_task = true;
        if (xTaskCreate(lib_daemon_task, "usb_host_lib", 4096, NULL, 6, NULL) == pdPASS) {
            s_has_daemon = true;
            err = usb_host_lib_set_root_port_power(true);
            if (err == ESP_OK) {
                return ESP_OK;
            }
        }
    }
    ESP_RETURN_ON_ERROR(drydock_usb_host_stop(), TAG, "rollback startup");
    return err;
}

esp_err_t drydock_usb_host_stop(void)
{
    if (s_client == NULL && !s_has_daemon) {
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupSetBits(s_lifecycle, STOP_REQUESTED);
    if (s_has_client_task) {
        ESP_RETURN_ON_ERROR(usb_host_client_unblock(s_client), TAG, "unblock client");
        xEventGroupWaitBits(s_lifecycle, CLIENT_DONE, pdFALSE, pdTRUE, portMAX_DELAY);
        ESP_RETURN_ON_ERROR(s_client_result, TAG, "stop client");
        s_has_client_task = false;
    }
    if (s_client != NULL) {
        ESP_RETURN_ON_ERROR(usb_host_client_deregister(s_client), TAG, "deregister client");
        s_client = NULL;
    }
    if (s_has_daemon) {
        xEventGroupWaitBits(s_lifecycle, DAEMON_DONE, pdFALSE, pdTRUE, portMAX_DELAY);
        ESP_RETURN_ON_ERROR(s_daemon_result, TAG, "free devices");
        s_has_daemon = false;
    } else {
        // Startup rollback: the port is still unpowered, but deregister queued NO_CLIENTS.
        ESP_RETURN_ON_ERROR(usb_host_lib_handle_events(0, NULL), TAG, "drain host events");
    }
    return usb_host_uninstall();
}

static const usb_intf_desc_t *find_interface(const usb_config_desc_t *cfg, int class_,
                                               int subclass, int proto, bool require_endpoints)
{
    if (cfg == NULL) {
        return NULL;
    }
    const uint8_t *data = (const uint8_t *)cfg;
    const size_t total = cfg->wTotalLength;
    for (size_t offset = cfg->bLength; offset + 2 <= total;) {
        const uint8_t *p = data + offset;
        const size_t len = p[0];
        if (len < 2 || len > total - offset) {
            break;
        }
        offset += len;
        if (p[1] != USB_W_VALUE_DT_INTERFACE || len < USB_INTF_DESC_SIZE) {
            continue;
        }
        const usb_intf_desc_t *iface = (const usb_intf_desc_t *)p;
        const int c_ok = (class_ < 0 || iface->bInterfaceClass == class_);
        const int s_ok = (subclass < 0 || iface->bInterfaceSubClass == subclass);
        const int p_ok = (proto < 0 || iface->bInterfaceProtocol == proto);
        if (c_ok && s_ok && p_ok &&
            (!require_endpoints ||
             (drydock_usb_find_ep(cfg, iface, false) && drydock_usb_find_ep(cfg, iface, true)))) {
            return iface;
        }
    }
    return NULL;
}

const usb_intf_desc_t *drydock_usb_find_interface(const usb_config_desc_t *cfg,
                                                  int class_, int subclass, int proto)
{
    return find_interface(cfg, class_, subclass, proto, false);
}

const usb_intf_desc_t *drydock_usb_find_data_iface(const usb_config_desc_t *cfg,
                                                   int class_, int subclass, int proto)
{
    return find_interface(cfg, class_, subclass, proto, true);
}

const usb_ep_desc_t *drydock_usb_find_ep(const usb_config_desc_t *cfg,
                                          const usb_intf_desc_t *iface, bool dir_out)
{
    const uint8_t *data = (const uint8_t *)cfg;
    const size_t total = cfg->wTotalLength;
    size_t endpoints = iface->bNumEndpoints;
    for (size_t offset = (const uint8_t *)iface - data + iface->bLength;
         endpoints > 0 && offset + 2 <= total;) {
        const uint8_t *p = data + offset;
        const size_t len = p[0];
        if (len < 2 || len > total - offset || p[1] == USB_W_VALUE_DT_INTERFACE) {
            break;
        }
        offset += len;
        if (p[1] == USB_W_VALUE_DT_ENDPOINT && len >= USB_EP_DESC_SIZE) {
            endpoints--;
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)p;
            const bool is_out = (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) == 0;
            if (is_out == dir_out) {
                return ep;
            }
        }
    }
    return NULL;
}
