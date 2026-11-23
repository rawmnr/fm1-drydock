/* FM1-Drydock — scaffold & smoke harness (Waveshare ESP32-S3-Touch-AMOLED-1.8).
 *
 * Build par défaut : smoke 1 (écran + tactile + détection de révision + PMU AXP2101)
 * puis smoke 2 (microSD en lecture seule, layout drydock).
 * Build smoke 3 (probe USB Host -> VID/PID sur l'AMOLED) :
 *     idf.py -DSMOKE_USB_HOST=1 build
 */
#include <stdarg.h>
#include <stdint.h>
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

/* I2C 0 : tactile (V2: CST816/CST820 0x15 ; V1: FT3168 0x38), expander TCA9554 0x20, PMU AXP2101 0x34 */
#define I2C_PORT     I2C_NUM_0
#define I2C_SDA      GPIO_NUM_15
#define I2C_SCL      GPIO_NUM_14
#define I2C_FREQ_HZ  400000
#define CST816_ADDR  0x15
#define FT3168_ADDR  0x38

#define AXP2101_ADDR         0x34
#define AXP2101_ADC_CTRL     0x30
#define AXP2101_ADC_BATV_H   0x34
#define AXP2101_ADC_BATV_L   0x35
#define AXP2101_STATUS1      0x00
#define AXP2101_STATUS2      0x01
#define AXP2101_BAT_PERCENT  0xA4

#define DRYDOCK_DIR  "/sdcard/drydock"
#define MANIFEST     DRYDOCK_DIR "/manifest.json"

static lv_obj_t *s_body; /* label non const, mis à jour sous lock */
static char s_status[512] = {0};

/* déclarés avant use (probe_board / probe_sdcard / usb_client_task sont définies plus bas) */
static void status_add(const char *fmt, ...);
static void refresh_display(void);

typedef struct {
    int percent;       /* 0..100 ; -1 si inconnu */
    int charging;
    int vbus_present;
    uint32_t bat_raw;  /* ADC 13 bits brut */
    uint32_t bat_mv;   /* estimation raw * 1,4 mV/LSB (à confirmer au test matinal) */
    int present;       /* 1 si AXP2101 répond en I2C */
} pmu_telemetry_t;

static i2c_master_dev_handle_t pmu_dev;

static uint8_t i2c_read8(i2c_master_dev_handle_t dev, uint8_t reg)
{
    uint8_t out = 0;
    if (i2c_master_transmit_receive(dev, &reg, 1, &out, 1, 50) == ESP_OK) {
        return out;
    }
    return 0;
}

static esp_err_t open_pmu(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(bus, &cfg, &pmu_dev);
}

static pmu_telemetry_t read_pmu(void)
{
    pmu_telemetry_t t = { .percent = -1, .charging = 0, .vbus_present = 0, .bat_raw = 0, .bat_mv = 0, .present = 1 };

    /* active l'ADC tension batterie (bit0 de 0x30) si besoin */
    uint8_t ctrl = i2c_read8(pmu_dev, AXP2101_ADC_CTRL);
    uint8_t enable[2] = { AXP2101_ADC_CTRL, (uint8_t)(ctrl | 0x01) };
    i2c_master_transmit(pmu_dev, enable, sizeof(enable), 50);
    vTaskDelay(pdMS_TO_TICKS(30));

    const uint8_t hi = i2c_read8(pmu_dev, AXP2101_ADC_BATV_H);
    const uint8_t lo = i2c_read8(pmu_dev, AXP2101_ADC_BATV_L);
    t.bat_raw = ((uint32_t)(hi & 0x1F) << 8) | lo; /* ADC 13 bits ; échelle mV à calibrer (datasheet) */

    const uint8_t s1 = i2c_read8(pmu_dev, AXP2101_STATUS1);
    const uint8_t s2 = i2c_read8(pmu_dev, AXP2101_STATUS2);
    t.vbus_present = (s1 >> 5) & 1;
    t.charging = ((s2 >> 5) == 0x01) ? 1 : 0;
    t.percent = i2c_read8(pmu_dev, AXP2101_BAT_PERCENT);
    return t;
}

/* Smoke 1 : révision de carte par probe des tactiles (méthode Waveshare) et télémétrie PMU. */
static void probe_board(i2c_master_bus_handle_t bus)
{
    const bool is_v2 = (i2c_master_probe(bus, CST816_ADDR, 50) == ESP_OK);
    const bool is_v1 = (i2c_master_probe(bus, FT3168_ADDR, 50) == ESP_OK);
    const char *rev = is_v2 ? "V2 (CO5300 + CST816/CST820)"
                   : is_v1 ? "V1 (SH8601 + FT3168)"
                           : "unknown";
    ESP_LOGI(TAG, "board revision: %s (cst816=%d ft3168=%d)", rev, is_v2, is_v1);

    const esp_err_t pmu_ok = open_pmu(bus);
    if (pmu_ok == ESP_OK) {
        const pmu_telemetry_t p = read_pmu();
        ESP_LOGI(TAG, "AXP2101: batt=%d%% chg=%d vbus=%d adc_raw=%u",
                 p.percent, p.charging, p.vbus_present, p.bat_raw);
        status_add("battery: %d%% %s VBUS:%s", p.percent,
                   p.charging ? "(charge)" : "",
                   p.vbus_present ? "on" : "off");
    } else {
        ESP_LOGW(TAG, "AXP2101 introuvable sur I2C 0x%02x: %s", AXP2101_ADDR, esp_err_to_name(pmu_ok));
        status_add("battery: n/a (pas d'AXP2101)");
    }
    status_add("board: %s", rev);
}

/* Smoke 2 : microSD en lecture seule, layout drydock. */
static void probe_sdcard(void)
{
    esp_err_t ret = bsp_sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "SD mount: %s (carte absente ou échec)", esp_err_to_name(ret));
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
    if (usb_host_client_register(&cfg, &client) != ESP_OK) {
        ESP_LOGE(TAG, "usb client register failed");
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
    if (!lv_display_get_default() || s_body == NULL) {
        return;
    }
    if (bsp_display_lock(1000)) {
        lv_label_set_text(s_body, s_status);
        bsp_display_unlock();
    }
}

static void build_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_scroll_dir(scr, LV_DIR_NONE);

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
    ESP_LOGI(TAG, "chip v%d.%d, flash %u MB, psram %s",
             chip.revision / 100, chip.revision % 100,
             (unsigned)flash_size / (1024 * 1024),
             esp_psram_is_initialized() ? "OK" : "NONE");

    /* Probe I2C (révision + PMU) avant que le BSP ne monopolise le bus. */
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bus_cfg, &bus) == ESP_OK) {
        probe_board(bus);
        i2c_del_master_bus(bus);
    } else {
        ESP_LOGW(TAG, "i2c master bus indisponible");
    }

    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start a échoué");
        abort();
    }
    ESP_LOGI(TAG, "display + touch + LVGL prêts (368x448)");
    ESP_ERROR_CHECK(bsp_display_brightness_set(70));
    ESP_LOGI(TAG, "brightness 70%");

    if (bsp_display_lock(1000)) {
        build_screen();
        refresh_display();
        bsp_display_unlock();
    }

    probe_sdcard();
    refresh_display();

#ifdef SMOKE_USB_HOST
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