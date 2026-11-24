/* FM1-Drydock — firmware de la cartouche (Waveshare ESP32-S3-Touch-AMOLED-1.8).
 *
 * Boot dual-mode : HOST_FLASH (défaut) monte /drydock et pilote le FM-1 en
 * USB Host (MIDI M-UPGRADE / Rescue UBOOT1.00) ; DEVICE_PC_SYNC expose la
 * partition FAT au PC en USB MSC (TinyUSB), sans VFS local.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "boot_mode.h"
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "drydock_storage.h"
#include "drydock_usb_host.h"
#include "drydock_usb_midi_host.h"
#include "drydock_usb_msc_device.h"
#include "drydock_usb_rescue_host.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

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

#define DRYDOCK_DIR  "/drydock"
#define MANIFEST     DRY_MANIFEST

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
    int ok;            /* 1 si toutes les lectures télémétrie ont réussi */
} pmu_telemetry_t;

static i2c_master_dev_handle_t pmu_dev;

static uint8_t i2c_read8(i2c_master_dev_handle_t dev, uint8_t reg, int *ok)
{
    uint8_t out = 0;
    if (i2c_master_transmit_receive(dev, &reg, 1, &out, 1, 50) == ESP_OK) {
        return out;
    }
    *ok = 0; /* lecture ratée : ne pas présenter un 0 comme une valeur réelle */
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
    pmu_telemetry_t t = { .percent = -1, .charging = 0, .vbus_present = 0, .bat_raw = 0, .ok = 1 };

    /* active l'ADC tension batterie (bit0 de 0x30) si besoin */
    uint8_t ctrl = i2c_read8(pmu_dev, AXP2101_ADC_CTRL, &t.ok);
    uint8_t enable[2] = { AXP2101_ADC_CTRL, (uint8_t)(ctrl | 0x01) };
    if (i2c_master_transmit(pmu_dev, enable, sizeof(enable), 50) != ESP_OK) {
        t.ok = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(30));

    const uint8_t hi = i2c_read8(pmu_dev, AXP2101_ADC_BATV_H, &t.ok);
    const uint8_t lo = i2c_read8(pmu_dev, AXP2101_ADC_BATV_L, &t.ok);
    t.bat_raw = ((uint32_t)(hi & 0x1F) << 8) | lo; /* ADC 13 bits ; échelle mV à calibrer (datasheet) */

    const uint8_t s1 = i2c_read8(pmu_dev, AXP2101_STATUS1, &t.ok);
    const uint8_t s2 = i2c_read8(pmu_dev, AXP2101_STATUS2, &t.ok);
    t.vbus_present = (s1 >> 5) & 1;
    t.charging = ((s2 >> 5) == 0x01) ? 1 : 0;
    t.percent = i2c_read8(pmu_dev, AXP2101_BAT_PERCENT, &t.ok);

    if (!t.ok) { /* n'affiche pas de zéros trompeurs */
        t.percent = -1;
        t.charging = 0;
        t.vbus_present = 0;
        t.bat_raw = 0;
    }
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
        if (!p.ok) {
            ESP_LOGW(TAG, "AXP2101: lecture télémétrie ratée");
            status_add("battery: n/a (lecture AXP ratée)");
        } else {
            ESP_LOGI(TAG, "AXP2101: batt=%d%% chg=%d vbus=%d adc_raw=%u",
                     p.percent, p.charging, p.vbus_present, p.bat_raw);
            status_add("battery: %d%% %s VBUS:%s", p.percent,
                       p.charging ? "(charge)" : "",
                       p.vbus_present ? "on" : "off");
        }
    } else {
        ESP_LOGW(TAG, "AXP2101 introuvable sur I2C 0x%02x: %s", AXP2101_ADDR, esp_err_to_name(pmu_ok));
        status_add("battery: n/a (pas d'AXP2101)");
    }
    status_add("board: %s", rev);
}

/* Smoke 2 : partition FAT interne (via drydock_storage, Wear Levelling). */
static void probe_sdcard(void)
{
    const esp_err_t ret = drydock_sd_mount();
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "FAT interne: mount %s", esp_err_to_name(ret));
        status_add("FS: %s", esp_err_to_name(ret));
        return;
    }
    uint64_t free_b = 0;
    if (drydock_fs_free(&free_b) == ESP_OK) {
        ESP_LOGI(TAG, "FAT interne: monté sur %s, libre %llu Mo",
                 DRY_MOUNT_POINT, (unsigned long long)(free_b >> 20));
        status_add("FS: %llu Mo libres", (unsigned long long)(free_b >> 20));
    }
    drydock_manifest_t m;
    if (drydock_manifest_parse(MANIFEST, &m) == 0) {
        ESP_LOGI(TAG, "manifest OK: %d vessels, stock=%s sha=%s",
                 m.vessel_count, m.stock_file, m.stock_sha256);
        status_add("FS: %d vessels, stock OK", m.vessel_count);
    } else {
        ESP_LOGW(TAG, "manifest absent/invalide (%s)", MANIFEST);
        status_add("FS: pas de manifest valide");
    }
    drydock_sd_unmount();
}

/* Moteurs USB Host (MIDI M-UPGRADE + Rescue UBOOT1.00) sur le FM-1. */
static void usb_host_start(void)
{
    const drydock_usb_driver_t drivers[] = {
        *drydock_midi_driver(),
        *drydock_rescue_driver(),
    };
    const esp_err_t ret =
        drydock_usb_host_start(drivers, sizeof(drivers) / sizeof(drivers[0]));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_start: %s", esp_err_to_name(ret));
        status_add("USB Host: %s", esp_err_to_name(ret));
        return;
    }
    status_add("USB Host: FM-1 détecté -> flash MIDI / rescue UBOOT1.00");
    refresh_display();
}

/* ---- Bascule de mode : bouton tactile + commande console ---- */

static void switch_mode_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "bouton tactile: bascule de mode");
    const boot_mode_t next = boot_mode_current() == BOOT_MODE_PC_SYNC ? BOOT_MODE_HOST_FLASH
                                                                      : BOOT_MODE_PC_SYNC;
    boot_mode_switch(next);
}

static int console_cmd_pc_sync(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    boot_mode_switch(BOOT_MODE_PC_SYNC);
    return 0;
}

static int console_cmd_host(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    boot_mode_switch(BOOT_MODE_HOST_FLASH);
    return 0;
}

static void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "drydock>";

    const esp_console_cmd_t cmds[] = {
        {
            .command = "pc_sync",
            .help = "basculer en mode DEVICE_PC_SYNC (MSC) puis redémarrer",
            .func = &console_cmd_pc_sync,
        },
        {
            .command = "host",
            .help = "basculer en mode HOST_FLASH puis redémarrer",
            .func = &console_cmd_host,
        },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }

#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &repl_cfg, &repl));
#else
    esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw, &repl_cfg, &repl));
#endif
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "console prête : 'pc_sync' | 'host'");
}

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

    /* Bouton de bascule de mode (tactile) : PC SYNC <-> HOST_FLASH. */
    const bool pc_sync = boot_mode_current() == BOOT_MODE_PC_SYNC;
    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 180, 44);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(btn, pc_sync ? lv_color_hex(0x2f9bff) : lv_color_hex(0x1b7a2e),
                              LV_PART_MAIN);
    lv_obj_add_event_cb(btn, switch_mode_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, pc_sync ? "DONE & Reboot" : "PC SYNC");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_center(lbl);
}

void app_main(void)
{
    ESP_LOGI(TAG, "FM1-Drydock firmware scaffold");
    ESP_LOGI(TAG, "boot mode: %s",
             boot_mode_current() == BOOT_MODE_PC_SYNC ? "PC_SYNC" : "HOST_FLASH");

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

    if (boot_mode_current() == BOOT_MODE_PC_SYNC) {
        /* Exclusion mutuelle : PAS de VFS local, le bloc spiflash part au PC. */
        ESP_LOGI(TAG, "DEVICE_PC_SYNC : pas de montage VFS, exposition MSC");
        wl_handle_t wl = WL_INVALID_HANDLE;
        ESP_ERROR_CHECK(drydock_fs_wl_init(&wl));
        ESP_ERROR_CHECK(drydock_msc_start(wl));
        status_add("PC SYNC: volume DRYDOCK exposé");
        status_add("Toucher DONE ou débrancher pour revenir");
        refresh_display();
        int gone = 0;
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(500));
            /* l'hôte est parti : débranchement (tud_connected) ou éjection SCSI
             * (le montage MSC repasse côté app, plus exposé au PC). */
            const bool host_here = drydock_msc_host_connected() && drydock_msc_is_exposed();
            if (host_here) {
                gone = 0;
                continue;
            }
            /* retour au dock même si la batterie maintient l'alimentation (flag RTC conservé) */
            if (++gone >= 8) { /* ~4 s sans hôte */
                ESP_LOGW(TAG, "hôte USB parti (débranchement/éjection): retour HOST_FLASH");
                boot_mode_switch(BOOT_MODE_HOST_FLASH);
            }
        }
    }

    probe_sdcard();
    refresh_display();
    console_start();

    /* Moteurs USB Host : MIDI M-UPGRADE (flash) + Rescue UBOOT1.00 (SCSI). */
    ESP_ERROR_CHECK(drydock_midi_init(NULL));
    ESP_ERROR_CHECK(drydock_rescue_init(NULL));
    usb_host_start();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "alive: internal=%u psram=%u",
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}