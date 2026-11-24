#include "drydock_usb_msc_device.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "tusb.h"

static const char *TAG = "dry_msc";

static tinyusb_msc_storage_handle_t s_storage_hdl = NULL;

/* TinyUSB descriptors: Device + MSC (the FAT volume carries the DRYDOCK label,
 * drydock-sync detects it by its volume name, not by PID). */
#define EPNUM_MSC 1
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

enum {
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL,
};

enum {
    EDPT_MSC_OUT = 0x01,
    EDPT_MSC_IN = 0x81,
};

static tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A, /* Espressif */
    .idProduct = 0x4002,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const uint8_t s_msc_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 64),
};

static const char *s_string_desc[] = {
    (const char[]){ 0x09, 0x04 }, /* 0: language (0x0409 English) */
    "FM1-Drydock",                /* 1: manufacturer */
    "DRYDOCK",                    /* 2: product */
    "0001",                       /* 3: serial */
};

bool drydock_msc_host_connected(void)
{
    return tud_connected();
}

int drydock_msc_is_exposed(void)
{
    tinyusb_msc_mount_point_t mp = TINYUSB_MSC_STORAGE_MOUNT_APP;
    if (s_storage_hdl == NULL) {
        return 0;
    }
    if (tinyusb_msc_get_storage_mount_point(s_storage_hdl, &mp) != ESP_OK) {
        return 0;
    }
    return mp == TINYUSB_MSC_STORAGE_MOUNT_USB ? 1 : 0;
}

esp_err_t drydock_msc_start(wl_handle_t wl_handle)
{
    /* The storage is created exposed to the PC (MOUNT_USB): the MCU doesn't access it. */
    const tinyusb_msc_storage_config_t storage_cfg = {
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        .fat_fs = {
            .base_path = NULL,
            .config.max_files = 0,
            .format_flags = 0,
        },
        .medium = { .wl_handle = wl_handle },
    };
    esp_err_t ret = tinyusb_msc_new_storage_spiflash(&storage_cfg, &s_storage_hdl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_new_storage_spiflash: %s", esp_err_to_name(ret));
        return ret;
    }

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &s_device_desc;
    tusb_cfg.descriptor.full_speed_config = s_msc_config_desc;
    tusb_cfg.descriptor.string = s_string_desc;
    tusb_cfg.descriptor.string_count = sizeof(s_string_desc) / sizeof(s_string_desc[0]);
    ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "USB MSC active: volume DRYDOCK exposed to the PC (12 MB)");
    return ESP_OK;
}
