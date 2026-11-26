/* FM1-Drydock — cartridge firmware (Waveshare ESP32-S3-Touch-AMOLED-1.8).
 *
 * Dual-mode boot: HOST_FLASH (default) mounts /drydock and drives the FM-1 in
 * USB Host (MIDI M-UPGRADE / Rescue UBOOT1.00); DEVICE_PC_SYNC exposes the
 * FAT partition to the PC via USB MSC (TinyUSB), without local VFS.
 */
#include <stdio.h>
#include <string.h>

#include "boot_mode.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "driver/i2c_master.h"
#include "drydock_storage.h"
#include "drydock_ui.h"
#include "drydock_usb_host.h"
#include "drydock_usb_probe.h"
#include "drydock_usb_midi_host.h"
#include "drydock_usb_msc_device.h"
#include "drydock_usb_rescue_host.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
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

typedef struct {
    int percent;       /* 0..100; -1 if unknown */
    int charging;
    int vbus_present;
    uint32_t bat_raw;  /* raw 13-bit ADC */
    int ok;            /* 1 if all telemetry reads succeeded */
} pmu_telemetry_t;

static i2c_master_dev_handle_t pmu_dev;

static uint8_t i2c_read8(i2c_master_dev_handle_t dev, uint8_t reg, int *ok)
{
    uint8_t out = 0;
    if (i2c_master_transmit_receive(dev, &reg, 1, &out, 1, 50) == ESP_OK) {
        return out;
    }
    *ok = 0; /* read failed: do not present a 0 as a real value */
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

    /* enable the battery voltage ADC (bit0 of 0x30) if needed */
    uint8_t ctrl = i2c_read8(pmu_dev, AXP2101_ADC_CTRL, &t.ok);
    uint8_t enable[2] = { AXP2101_ADC_CTRL, (uint8_t)(ctrl | 0x01) };
    if (i2c_master_transmit(pmu_dev, enable, sizeof(enable), 50) != ESP_OK) {
        t.ok = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(30));

    const uint8_t hi = i2c_read8(pmu_dev, AXP2101_ADC_BATV_H, &t.ok);
    const uint8_t lo = i2c_read8(pmu_dev, AXP2101_ADC_BATV_L, &t.ok);
    t.bat_raw = ((uint32_t)(hi & 0x1F) << 8) | lo; /* 13-bit ADC; mV scale to calibrate (datasheet) */

    const uint8_t s1 = i2c_read8(pmu_dev, AXP2101_STATUS1, &t.ok);
    const uint8_t s2 = i2c_read8(pmu_dev, AXP2101_STATUS2, &t.ok);
    t.vbus_present = (s1 >> 5) & 1;
    t.charging = ((s2 >> 5) == 0x01) ? 1 : 0;
    t.percent = i2c_read8(pmu_dev, AXP2101_BAT_PERCENT, &t.ok);

    if (!t.ok) { /* do not show misleading zeros */
        t.percent = -1;
        t.charging = 0;
        t.vbus_present = 0;
        t.bat_raw = 0;
    }
    return t;
}

/* Board revision by probing touch controllers (Waveshare method) + PMU telemetry. */
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
            ESP_LOGW(TAG, "AXP2101: telemetry read failed");
        } else {
            ESP_LOGI(TAG, "AXP2101: batt=%d%% chg=%d vbus=%d adc_raw=%u",
                     p.percent, p.charging, p.vbus_present, p.bat_raw);
        }
    } else {
        ESP_LOGW(TAG, "AXP2101 not found on I2C 0x%02x: %s", AXP2101_ADDR, esp_err_to_name(pmu_ok));
    }
}

/* Internal FAT partition (via drydock_storage, Wear Levelling). */
static void probe_sdcard(void)
{
    const esp_err_t ret = drydock_sd_mount();
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "internal FAT: mount %s", esp_err_to_name(ret));
        return;
    }
    uint64_t free_b = 0;
    if (drydock_fs_free(&free_b) == ESP_OK) {
        ESP_LOGI(TAG, "internal FAT: mounted at %s, free %llu MB",
                 DRY_MOUNT_POINT, (unsigned long long)(free_b >> 20));
    }
    drydock_manifest_t m;
    if (drydock_manifest_parse(DRY_MANIFEST, &m) == 0) {
        ESP_LOGI(TAG, "manifest OK: %d vessels, stock=%s sha=%s",
                 m.vessel_count, m.stock_file, m.stock_sha256);
    } else {
        ESP_LOGW(TAG, "manifest missing/invalid (%s)", DRY_MANIFEST);
    }
    /* stays mounted: flashing reads packets from /drydock at runtime */
}

/* USB Host engines (MIDI M-UPGRADE + Rescue UBOOT1.00) on the FM-1.
 * The electrical probe runs first, while the pads are still free: it tells
 * why the FM-1 does not show up (charge-only cable, FM-1 off / battery low,
 * FM-1 left in download mode, shorted cable) before enumeration is tried. */
static void usb_host_start(void)
{
    drydock_bus_probe_t probe;
    if (drydock_usb_bus_probe(&probe) == ESP_OK &&
        probe.presence != DRYDOCK_BUS_DEVICE) {
        ESP_LOGW(TAG, "USB bus: %s — %s",
                 drydock_bus_presence_name(probe.presence),
                 drydock_bus_presence_hint(probe.presence));
    }
    const drydock_usb_driver_t drivers[] = {
        *drydock_midi_driver(),
        *drydock_rescue_driver(),
    };
    const esp_err_t ret =
        drydock_usb_host_start(drivers, sizeof(drivers) / sizeof(drivers[0]));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_start: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "USB Host active: FM-1 MIDI (M-UPGRADE) / UBOOT1.00 (rescue)");
}

/* ---- commandes console ---- */

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
            .help = "switch to DEVICE_PC_SYNC (MSC) mode then reboot",
            .func = &console_cmd_pc_sync,
        },
        {
            .command = "host",
            .help = "switch to HOST_FLASH mode then reboot",
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
    ESP_LOGI(TAG, "console ready: 'pc_sync' | 'host'");
}

static void display_round_area(lv_area_t *area)
{
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

static void display_start(void)
{
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_touch_handle_t touch;
    const bsp_display_config_t panel_cfg = {0};
    ESP_ERROR_CHECK(bsp_display_new(&panel_cfg, &panel, &io));
    /* The touch probe sets the panel's revision-specific X offset. */
    ESP_ERROR_CHECK(bsp_touch_new(NULL, &touch));
    ESP_ERROR_CHECK(bsp_display_brightness_set(70));

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        /* Keep the RGB565 tile below the ESP32-S3's 32 KiB SPI DMA limit. */
        .buffer_size = BSP_LCD_H_RES * 40,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .rounder_cb = display_round_area,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    /* This is a QSPI panel, not RGB parallel as in bsp_display_start(). */
    lv_display_t *disp = lvgl_port_add_disp(&display_cfg);
    ESP_ERROR_CHECK(disp != NULL ? ESP_OK : ESP_FAIL);
    const lvgl_port_touch_cfg_t touch_cfg = {.disp = disp, .handle = touch};
    ESP_ERROR_CHECK(lvgl_port_add_touch(&touch_cfg) != NULL ? ESP_OK : ESP_FAIL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "FM1-Drydock firmware");
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

    /* Probe I2C (revision + PMU) before the BSP monopolizes the bus. */
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
        ESP_LOGW(TAG, "i2c master bus unavailable");
    }

    display_start();
    ESP_LOGI(TAG, "display + touch + LVGL ready (368x448)");

    if (boot_mode_current() == BOOT_MODE_PC_SYNC) {
        /* Mutual exclusion: no local VFS, the spiflash block goes to the PC. */
        ESP_LOGI(TAG, "DEVICE_PC_SYNC: no VFS mount, exposing MSC");
        wl_handle_t wl = WL_INVALID_HANDLE;
        ESP_ERROR_CHECK(drydock_fs_wl_init(&wl));
        ESP_ERROR_CHECK(drydock_msc_start(wl));

        if (bsp_display_lock(1000)) {
            drydock_ui_start();
            bsp_display_unlock();
        }

        int gone = 0;
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(500));
            /* host is gone: unplug (tud_connected) or SCSI eject
             * (the MSC mount goes back to the app side, no longer exposed to the PC). */
            const bool host_here = drydock_msc_host_connected() && drydock_msc_is_exposed();
            if (host_here) {
                gone = 0;
                continue;
            }
            /* return to dock even if the battery keeps supplying power (RTC flag preserved) */
            if (++gone >= 8) { /* ~4 s without host */
                ESP_LOGW(TAG, "USB host gone (unplug/eject): back to HOST_FLASH");
                boot_mode_switch(BOOT_MODE_HOST_FLASH);
            }
        }
    }

    probe_sdcard();
    console_start();

    /* USB Host engines: MIDI M-UPGRADE (flash) + Rescue UBOOT1.00 (SCSI). */
    ESP_ERROR_CHECK(drydock_midi_init(NULL));
    ESP_ERROR_CHECK(drydock_rescue_init(NULL));
    usb_host_start();

    if (bsp_display_lock(1000)) {
        drydock_ui_start();
        bsp_display_unlock();
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "alive: internal=%u psram=%u",
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}
