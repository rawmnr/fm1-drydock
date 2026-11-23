/* FM1-Drydock — scaffold & smoke harness (Waveshare ESP32-S3-Touch-AMOLED-1.8).
 *
 * Build par défaut : smoke 1 (écran + tactile + détection de révision) puis smoke 2 (microSD).
 * Build smoke 3 (probe USB Host -> VID/PID sur l'AMOLED) :
 *     idf.py -DSMOKE_USB_HOST=1 build
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#ifdef SMOKE_USB_HOST
#include "esp_intr_alloc.h"
#include "usb/usb_host.h"
#endif

static const char *TAG = "drydock";

/* I2C 0 : tactile (CST816/CST820 sur V2, FT3168 sur V1) + expander TCA9554 */
#define I2C_PORT     I2C_NUM_0
#define I2C_SDA      GPIO_NUM_15
#define I2C_SCL      GPIO_NUM_14
#define CST816_ADDR  0x15

#define DRYDOCK_DIR  "/sdcard/drydock"
#define MANIFEST     DRYDOCK_DIR "/manifest.json"

static lv_obj_t *s_body;
static char s_status[512] = {0};

static void status_add(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char tmp[160];
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    size_t used = strlen(s_status);
    snprintf(s_status + used, sizeof(s_status) - used, "%s\n", tmp);
}

static void refresh_display(void)
{
    if (!lv_display_get_default()) {
        return;
    }
    if (bsp_display_lock(1000)) {
        lv_label_set_text(s_body, s_status);
        bsp_display_unlock();
    }
}

/* Smoke 1 : détection de la révision de carte (probe I2C, méthode officielle Waveshare). */
static const char *detect_board_revision(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        return "unknown (i2c probe failed)";
    }
    const bool is_v2 = (i2c_master_probe(bus, CST816_ADDR, 50) == ESP_OK);
    i2c_del_master_bus(bus);
    return is_v2 ? "V2 (CO5300 + CST816/CST820)" : "V1 / original (pas de tactile CST816)";
}

/* Smoke 2 : microSD en lecture seule, layout drydock. */
static void probe_sdcard(void)
{
    esp_err_t ret = bsp_sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD mount skipped/failed: %s", esp_err_to_name(ret));
        status_add("SD: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "SD mounted at %s", BSP_SD_MOUNT_POINT);
    FILE *f = fopen(MANIFEST, "r");
    if (f != NULL) {
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fclose(f);
        ESP_LOGI(TAG, "%s present (%ld bytes), layout drydock OK", MANIFEST, size);
        status_add("SD: mounted, manifest %ld B", size);
    } else {
        ESP_LOGW(TAG, "%s absent (SD montée mais layout drydock non initialisé)", MANIFEST);
        status_add("SD: mounted, pas de manifest");
    }
    bsp_sdcard_unmount();
}

#ifdef SMOKE_USB_HOST
static volatile uint8_t s_new_dev_addr;
static volatile uint8_t s_dev_gone;

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        s_new_dev_addr = msg->new_dev.address;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        s_dev_gone = 1;
        break;
    default:
        break;
    }
}

static void usb_client_task(void *arg)
{
    (void)arg;
    const usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .event_cb = client_event_cb,
        .event_cb_arg = NULL,
    };
    usb_host_client_handle_t client = NULL;
    esp_err_t err = usb_host_client_register(&cfg, &client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "client register: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
    }
    while (1) {
        usb_host_client_handle_events(client, portMAX_DELAY);
        if (s_dev_gone) {
            s_dev_gone = 0;
            ESP_LOGW(TAG, "USB device disconnected");
            status_add("USB: débranché");
            refresh_display();
        }
        if (s_new_dev_addr) {
            uint8_t addr = s_new_dev_addr;
            s_new_dev_addr = 0;
            ESP_LOGI(TAG, "nouvel appareil USB à l'adresse %u", addr);
            usb_device_handle_t dev = NULL;
            if (usb_host_device_open(client, addr, &dev) == ESP_OK) {
                const usb_device_desc_t *desc = NULL;
                if (usb_host_get_device_descriptor(dev, &desc) == ESP_OK && desc != NULL) {
                    ESP_LOGI(TAG, "VID=%04X PID=%04X, classe 0x%02X",
                             desc->idVendor, desc->idProduct, desc->bDeviceClass);
                    status_add("USB Host: VID=%04X PID=%04X", desc->idVendor, desc->idProduct);
                    refresh_display();
                }
                usb_host_device_close(client, dev);
            } else {
                ESP_LOGW(TAG, "impossible d'ouvrir le dispositif addr %u", addr);
            }
        }
    }
}

static void usb_host_start(void)
{
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    xTaskCreate(usb_client_task, "usb_client", 4096, NULL, 5, NULL);
}
#endif /* SMOKE_USB_HOST */

static void build_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x08111f), LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "FM1-Drydock");
    lv_obj_set_style_text_color(title, lv_color_hex(0x2f9bff), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    s_body = lv_label_create(scr);
    lv_obj_set_style_text_color(s_body, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_body, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_body, 330);
    lv_obj_align(s_body, LV_ALIGN_TOP_MID, 0, 70);
}

void app_main(void)
{
    ESP_LOGI(TAG, "FM1-Drydock firmware scaffold");

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    status_add("board: %s", detect_board_revision());
    status_add("flash: %u MB, psram: %s",
               (unsigned)flash_size / (1024 * 1024),
               esp_psram_is_initialized() ? "yes" : "no");

    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start a échoué");
        abort();
    }
    ESP_ERROR_CHECK(bsp_display_brightness_set(70));

    if (bsp_display_lock(1000)) {
        build_screen();
        refresh_display();
        bsp_display_unlock();
    }

    /* Smoke 1 : confirmation tactile/écran + révision déjà affichés. */
    /* Smoke 2 : microSD (lecture seule). */
    probe_sdcard();
    refresh_display();

#ifdef SMOKE_USB_HOST
    /* Smoke 3 : probe USB Host. Débrancher la carte du PC, relier le FM-1 à son USB-C. */
    status_add("USB Host probe active");
    refresh_display();
    usb_host_start();
#endif

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "alive: internal=%u psram=%u",
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}