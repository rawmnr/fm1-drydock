/* Interface LVGL 9 de la cartouche (368x448 tactile).
 *
 * Écrans : accueil (liste des vessels), détail + flash (barre de progression),
 * Rescue & Stock, PC Sync (état MSC + Done & Reboot), USB Diag. Barre de
 * statut en haut : DRYDOCK, batterie AXP2101, état USB, espace libre.
 *
 * En mode DEVICE_PC_SYNC le VFS n'est pas monté : seul l'écran PC Sync est
 * construit (le manifest et la liste des vessels sont inaccessibles).
 */
#include "drydock_ui.h"

#include <stdio.h>
#include <string.h>

#include "boot_mode.h"
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "drydock_storage.h"
#include "drydock_usb_midi_host.h"
#include "drydock_usb_msc_device.h"
#include "drydock_usb_rescue_host.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#define SCR_W 368
#define SCR_H 448
#define BAR_H 30
#define BTN_H 40

#define AXP2101_ADDR 0x34
#define AXP2101_STATUS1 0x00
#define AXP2101_BAT_PERCENT 0xA4

static const char *TAG = "dry_ui";

static lv_obj_t *s_scr_home;
static lv_obj_t *s_scr_detail;
static lv_obj_t *s_scr_rescue;
static lv_obj_t *s_scr_sync;
static lv_obj_t *s_scr_diag;

/* barre de statut (labels recréés à chaque écran) */
static lv_obj_t *s_bar_batt;
static lv_obj_t *s_bar_usb;
static lv_obj_t *s_bar_fs;

/* écran détail */
static lv_obj_t *s_det_name;
static lv_obj_t *s_det_meta;
static lv_obj_t *s_det_desc;
static lv_obj_t *s_det_progress;
static lv_obj_t *s_det_progress_lbl;
static lv_obj_t *s_det_status;
static lv_obj_t *s_det_flash_btn;
static lv_obj_t *s_det_stock_first;
static drydock_vessel_t s_current_vessel;

/* écran rescue */
static lv_obj_t *s_res_status;

/* écran sync */
static lv_obj_t *s_sync_status;

/* écran diag */
static lv_obj_t *s_diag_text;

static volatile int s_flashing; /* 1 si un flash MIDI est en cours */

static void screen_back_cb(lv_event_t *e);

/* ---- helpers UI ---- */

static void status_bar(lv_obj_t *scr)
{
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, SCR_W, BAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x101828), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);

    lv_obj_t *dock = lv_label_create(bar);
    lv_label_set_text(dock, "DRYDOCK");
    lv_obj_set_style_text_color(dock, lv_color_hex(0x2f9bff), LV_PART_MAIN);
    lv_obj_set_style_text_font(dock, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(dock, LV_ALIGN_LEFT_MID, 8, 0);

    s_bar_batt = lv_label_create(bar);
    lv_obj_set_style_text_font(s_bar_batt, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_bar_batt, LV_ALIGN_RIGHT_MID, -8, 0);

    s_bar_usb = lv_label_create(bar);
    lv_obj_set_style_text_font(s_bar_usb, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_bar_usb, LV_ALIGN_RIGHT_MID, -52, 0);

    s_bar_fs = lv_label_create(bar);
    lv_obj_set_style_text_font(s_bar_fs, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_bar_fs, LV_ALIGN_RIGHT_MID, -110, 0);
}

static void nav_back(lv_obj_t *scr, const char *txt)
{
    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 70, BTN_H);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a3342), LV_PART_MAIN);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, screen_back_cb, LV_EVENT_CLICKED, NULL);
}

static void screen_show(lv_obj_t *scr)
{
    lv_screen_load(scr);
}

static void screen_back_cb(lv_event_t *e)
{
    (void)e;
    screen_show(s_scr_home);
}

static void refresh_status(void)
{
    char txt[48];

    /* batterie AXP2101 (bus I2C du BSP) */
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus != NULL) {
        const i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = AXP2101_ADDR,
            .scl_speed_hz = 400000,
        };
        i2c_master_dev_handle_t pmu = NULL;
        if (i2c_master_bus_add_device(bus, &cfg, &pmu) == ESP_OK) {
            uint8_t reg = AXP2101_BAT_PERCENT;
            uint8_t pct = 0;
            if (i2c_master_transmit_receive(pmu, &reg, 1, &pct, 1, 50) == ESP_OK) {
                snprintf(txt, sizeof(txt), "%d%%", pct);
            } else {
                snprintf(txt, sizeof(txt), "--");
            }
            i2c_master_bus_rm_device(pmu);
            if (s_bar_batt) {
                lv_label_set_text(s_bar_batt, txt);
            }
        }
    }

    /* état USB : FM-1 MIDI / rescue / absent */
    const char *usb = "--";
    if (drydock_rescue_connected()) {
        usb = "UBOOT";
    } else if (drydock_midi_connected()) {
        usb = "MIDI";
    }
    if (s_bar_usb) {
        lv_label_set_text(s_bar_usb, usb);
    }

    /* espace libre */
    uint64_t free_b = 0;
    if (drydock_fs_free(&free_b) == ESP_OK) {
        snprintf(txt, sizeof(txt), "%lluM", (unsigned long long)(free_b >> 20));
        if (s_bar_fs) {
            lv_label_set_text(s_bar_fs, txt);
        }
    }
}

/* ---- écran d'accueil (liste des vessels) ---- */

static void home_btn(lv_obj_t *scr, const char *txt, int idx, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, (SCR_W - 32) / 2, BTN_H);
    lv_obj_align(btn, idx % 2 ? LV_ALIGN_BOTTOM_RIGHT : LV_ALIGN_BOTTOM_LEFT,
                 idx % 2 ? -8 : 8, -8 - (idx / 2) * (BTN_H + 8));
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1b7a2e), LV_PART_MAIN);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
}

static void home_rescue_cb(lv_event_t *e)
{
    (void)e;
    screen_show(s_scr_rescue);
}

static void home_sync_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "bascule PC_SYNC (MSC) depuis l'UI");
    boot_mode_switch(BOOT_MODE_PC_SYNC);
}

static void home_diag_cb(lv_event_t *e)
{
    (void)e;
    screen_show(s_scr_diag);
}

static void vessel_click_cb(lv_event_t *e)
{
    const drydock_vessel_t *v = lv_event_get_user_data(e);
    if (v == NULL) {
        return;
    }
    s_current_vessel = *v;
    lv_label_set_text(s_det_name, v->name);
    char meta[96];
    snprintf(meta, sizeof(meta), "%s · v%s · %s", v->id, v->version, v->category);
    lv_label_set_text(s_det_meta, meta);
    char desc[160];
    snprintf(desc, sizeof(desc), "SHA-256: %.16s…\nTaille: %u Ko", v->sha256,
             (unsigned)(v->size_bytes / 1024));
    lv_label_set_text(s_det_desc, desc);
    lv_bar_set_value(s_det_progress, 0, LV_ANIM_OFF);
    lv_label_set_text(s_det_progress_lbl, "");
    lv_label_set_text(s_det_status, "");
    screen_show(s_scr_detail);
}

static void build_home(void)
{
    s_scr_home = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr_home, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_scr_home, LV_DIR_NONE);
    status_bar(s_scr_home);

    lv_obj_t *title = lv_label_create(s_scr_home);
    lv_label_set_text(title, "Vessels");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, BAR_H + 8);

    /* liste scrollable des vessels (flex column, lv_list est déprécié en v9) */
    lv_obj_t *list = lv_obj_create(s_scr_home);
    lv_obj_set_size(list, SCR_W - 16, 260);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, BAR_H + 36);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 0, LV_PART_MAIN);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    drydock_vessel_t *vessels = NULL;
    const int count = drydock_manifest_vessels(DRY_MANIFEST, &vessels);

    if (count == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, "Aucun vessel — PC Sync\npour synchroniser");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x888888), LV_PART_MAIN);
        lv_obj_center(empty);
    } else {
        for (int i = 0; i < count; i++) {
            char row[128];
            snprintf(row, sizeof(row), "%s  v%s  [%s]", vessels[i].name, vessels[i].version,
                     vessels[i].category);
            lv_obj_t *btn = lv_button_create(list);
            lv_obj_set_size(btn, SCR_W - 20, 44);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x141d2b), LV_PART_MAIN);
            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, row);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xdddddd), LV_PART_MAIN);
            lv_obj_center(lbl);
            lv_obj_add_event_cb(btn, vessel_click_cb, LV_EVENT_CLICKED, &vessels[i]);
        }
    }
    if (vessels != NULL) {
        free(vessels);
    }

    home_btn(s_scr_home, "Rescue / Stock", 0, home_rescue_cb);
    home_btn(s_scr_home, "PC Sync", 1, home_sync_cb);
    home_btn(s_scr_home, "USB Diag", 2, home_diag_cb);
}

/* ---- écran détail + flash ---- */

static void midi_state_cb(drydock_midi_state_t st, const char *detail);
static void midi_progress_cb(unsigned served, uint32_t addr);

static void flash_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_flashing) {
        return;
    }
    lv_label_set_text(s_det_status, "Vérification SHA-256…");
    char path[96];
    snprintf(path, sizeof(path), "vessels/%s", s_current_vessel.file);
    if (drydock_midi_flash(path) != ESP_OK) {
        lv_label_set_text(s_det_status, "Flash indisponible (MIDI ?)");
        return;
    }
    s_flashing = 1;
}

static void build_detail(void)
{
    s_scr_detail = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_detail, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_scr_detail, LV_DIR_NONE);
    status_bar(s_scr_detail);

    s_det_name = lv_label_create(s_scr_detail);
    lv_obj_set_style_text_color(s_det_name, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_det_name, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(s_det_name, LV_ALIGN_TOP_LEFT, 8, BAR_H + 10);

    s_det_meta = lv_label_create(s_scr_detail);
    lv_obj_set_style_text_color(s_det_meta, lv_color_hex(0x9aa4b2), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_det_meta, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_det_meta, LV_ALIGN_TOP_LEFT, 8, BAR_H + 36);

    s_det_desc = lv_label_create(s_scr_detail);
    lv_obj_set_style_text_color(s_det_desc, lv_color_hex(0xcccccc), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_det_desc, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_long_mode(s_det_desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_det_desc, SCR_W - 16);
    lv_obj_align(s_det_desc, LV_ALIGN_TOP_LEFT, 8, BAR_H + 62);

    s_det_stock_first = lv_checkbox_create(s_scr_detail);
    lv_checkbox_set_text(s_det_stock_first, "Flash Stock V15 d'abord");
    lv_obj_align(s_det_stock_first, LV_ALIGN_TOP_LEFT, 8, BAR_H + 170);
    lv_obj_add_state(s_det_stock_first, LV_STATE_CHECKED);

    s_det_progress = lv_bar_create(s_scr_detail);
    lv_obj_set_size(s_det_progress, SCR_W - 16, 14);
    lv_obj_align(s_det_progress, LV_ALIGN_TOP_MID, 0, BAR_H + 210);
    lv_bar_set_range(s_det_progress, 0, 100);

    s_det_progress_lbl = lv_label_create(s_scr_detail);
    lv_obj_set_style_text_font(s_det_progress_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_det_progress_lbl, LV_ALIGN_TOP_MID, 0, BAR_H + 230);

    s_det_status = lv_label_create(s_scr_detail);
    lv_obj_set_style_text_font(s_det_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_long_mode(s_det_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_det_status, SCR_W - 16);
    lv_obj_align(s_det_status, LV_ALIGN_TOP_LEFT, 8, BAR_H + 252);

    s_det_flash_btn = lv_button_create(s_scr_detail);
    lv_obj_set_size(s_det_flash_btn, 160, BTN_H);
    lv_obj_align(s_det_flash_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(s_det_flash_btn, lv_color_hex(0xc0392b), LV_PART_MAIN);
    lv_obj_t *lbl = lv_label_create(s_det_flash_btn);
    lv_label_set_text(lbl, "FLASH");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(s_det_flash_btn, flash_click_cb, LV_EVENT_CLICKED, NULL);

    nav_back(s_scr_detail, "< Accueil");
}

/* ---- Rescue & Stock ---- */

static void rescue_check_cb(lv_event_t *e)
{
    (void)e;
    drydock_rescue_info_t info;
    memset(&info, 0, sizeof(info));
    if (drydock_rescue_check(NULL, &info) != ESP_OK) {
        lv_label_set_text(s_res_status, "Pas de UBOOT1.00 (mode rescue)");
        return;
    }
    char txt[160];
    snprintf(txt, sizeof(txt), "%s %s\nFlash: %s · Clé: 0x%04X\nSecteurs à écrire: %u/%u",
             info.vendor, info.model, info.flash_ok ? "OK" : "?", info.chip_key,
             (unsigned)info.sectors_diff, (unsigned)info.sectors_total);
    lv_label_set_text(s_res_status, txt);
}

static void rescue_bootloader_cb(lv_event_t *e)
{
    (void)e;
    if (drydock_midi_send_enter_bootloader() != ESP_OK) {
        lv_label_set_text(s_res_status, "FM-1 MIDI absent");
        return;
    }
    lv_label_set_text(s_res_status, "Touche UBOOT envoyée…\nLe FM-1 passe en UBOOT1.00");
}

static void rescue_stock_cb(lv_event_t *e)
{
    (void)e;
    if (s_flashing) {
        return;
    }
    lv_label_set_text(s_res_status, "Retour Stock V15…");
    if (drydock_midi_flash(DRY_STOCK_FILE) != ESP_OK) {
        lv_label_set_text(s_res_status, "Flash indisponible (MIDI ?)");
        return;
    }
    s_flashing = 1;
}

static void build_rescue(void)
{
    s_scr_rescue = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_rescue, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_scr_rescue, LV_DIR_NONE);
    status_bar(s_scr_rescue);

    lv_obj_t *title = lv_label_create(s_scr_rescue);
    lv_label_set_text(title, "Rescue & Stock");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, BAR_H + 8);

    s_res_status = lv_label_create(s_scr_rescue);
    lv_obj_set_style_text_color(s_res_status, lv_color_hex(0xcccccc), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_res_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_long_mode(s_res_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_res_status, SCR_W - 16);
    lv_obj_align(s_res_status, LV_ALIGN_TOP_LEFT, 8, BAR_H + 40);

    /* colonne de boutons */
    const char *labels[] = { "Back to Stock V15", "Enter Bootloader", "Rescue check (UBOOT1.00)" };
    const lv_event_cb_t cbs[] = { rescue_stock_cb, rescue_bootloader_cb, rescue_check_cb };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = lv_button_create(s_scr_rescue);
        lv_obj_set_size(btn, SCR_W - 16, BTN_H);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, BAR_H + 130 + i * (BTN_H + 10));
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1b7a2e), LV_PART_MAIN);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, labels[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, cbs[i], LV_EVENT_CLICKED, NULL);
    }

    nav_back(s_scr_rescue, "< Accueil");
}

/* ---- PC Sync ---- */

static void sync_done_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Done: retour HOST_FLASH");
    boot_mode_switch(BOOT_MODE_HOST_FLASH);
}

static void build_sync(void)
{
    s_scr_sync = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_sync, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_scr_sync, LV_DIR_NONE);
    status_bar(s_scr_sync);

    lv_obj_t *title = lv_label_create(s_scr_sync);
    lv_label_set_text(title, "PC Sync");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, BAR_H + 8);

    s_sync_status = lv_label_create(s_scr_sync);
    lv_obj_set_style_text_color(s_sync_status, lv_color_hex(0xcccccc), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_sync_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_long_mode(s_sync_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_sync_status, SCR_W - 16);
    lv_obj_align(s_sync_status, LV_ALIGN_TOP_LEFT, 8, BAR_H + 40);

    lv_obj_t *done = lv_button_create(s_scr_sync);
    lv_obj_set_size(done, 200, BTN_H);
    lv_obj_align(done, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(done, lv_color_hex(0x2f9bff), LV_PART_MAIN);
    lv_obj_t *lbl = lv_label_create(done);
    lv_label_set_text(lbl, "Done (Reboot to Dock)");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(done, sync_done_cb, LV_EVENT_CLICKED, NULL);
}

/* ---- USB Diag ---- */

static void build_diag(void)
{
    s_scr_diag = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_diag, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_scr_diag, LV_DIR_NONE);
    status_bar(s_scr_diag);

    lv_obj_t *title = lv_label_create(s_scr_diag);
    lv_label_set_text(title, "USB Diag");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, BAR_H + 8);

    s_diag_text = lv_label_create(s_scr_diag);
    lv_obj_set_style_text_color(s_diag_text, lv_color_hex(0xcccccc), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_diag_text, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_long_mode(s_diag_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_diag_text, SCR_W - 16);
    lv_obj_align(s_diag_text, LV_ALIGN_TOP_LEFT, 8, BAR_H + 40);

    nav_back(s_scr_diag, "< Accueil");
}

/* ---- callbacks moteurs -> UI ---- */

static void midi_state_cb(drydock_midi_state_t st, const char *detail)
{
    (void)detail;
    if (!bsp_display_lock(500)) {
        return;
    }
    if (st == DRYDOCK_MIDI_DONE) {
        s_flashing = 0;
        if (s_det_status) {
            lv_label_set_text(s_det_status, "Flash terminé ✓");
        }
        if (s_res_status) {
            lv_label_set_text(s_res_status, "Stock V15 installé ✓");
        }
    } else if (st == DRYDOCK_MIDI_ERROR) {
        s_flashing = 0;
        if (s_det_status) {
            lv_label_set_text(s_det_status, "Erreur: MIDI absent ?");
        }
        if (s_res_status) {
            lv_label_set_text(s_res_status, "Erreur flash");
        }
    }
    bsp_display_unlock();
}

static void midi_progress_cb(unsigned served, uint32_t addr)
{
    (void)served;
    if (!bsp_display_lock(500)) {
        return;
    }
    /* addr = offset lu dans l'image logique ; on estime le % via la taille du vessel */
    const uint32_t img = s_current_vessel.size_bytes > 20 ? s_current_vessel.size_bytes - 20 : 1;
    const int pct = (int)((uint64_t)addr * 100u / img);
    if (s_det_progress) {
        lv_bar_set_value(s_det_progress, pct < 100 ? pct : 99, LV_ANIM_OFF);
    }
    if (s_det_progress_lbl) {
        char t[16];
        snprintf(t, sizeof(t), "%d%%", pct < 100 ? pct : 99);
        lv_label_set_text(s_det_progress_lbl, t);
    }
    bsp_display_unlock();
}

/* ---- tâche de rafraîchissement ---- */

static void refresh_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!bsp_display_lock(200)) {
            continue;
        }
        refresh_status();
        if (s_diag_text) {
            char txt[200];
            const char *usb = drydock_rescue_connected() ? "UBOOT1.00 (MSC)"
                           : drydock_midi_connected() ? "FM-1 (MIDI)"
                                                       : "aucun FM-1";
            snprintf(txt, sizeof(txt),
                     "Mode: %s\nFM-1: %s\nIdentité: %s\nVBUS: %s\nusb_host_lib: active",
                     boot_mode_current() == BOOT_MODE_PC_SYNC ? "PC_SYNC" : "HOST_FLASH",
                     usb, drydock_midi_identity(),
                     drydock_msc_host_connected() ? "on" : "off");
            lv_label_set_text(s_diag_text, txt);
        }
        if (s_sync_status) {
            char txt[160];
            const bool exposed = drydock_msc_host_connected() && drydock_msc_is_exposed();
            snprintf(txt, sizeof(txt),
                     "Volume DRYDOCK: %s\nPC branché: %s\n\nRetour au dock : bouton Done\nou débranchement",
                     exposed ? "exposé" : "non exposé",
                     drydock_msc_host_connected() ? "oui" : "non");
            lv_label_set_text(s_sync_status, txt);
        }
        bsp_display_unlock();
    }
}

/* ---- API ---- */

esp_err_t drydock_ui_start(void)
{
    const bool pc_sync = (boot_mode_current() == BOOT_MODE_PC_SYNC);

    if (pc_sync) {
        /* VFS non monté : écran PC Sync seul */
        build_sync();
        screen_show(s_scr_sync);
    } else {
        /* branche les callbacks de flash sur l'UI (init idempotente) */
        const drydock_midi_callbacks_t midi_cb = {
            .on_state = midi_state_cb,
            .on_progress = midi_progress_cb,
        };
        drydock_midi_init(&midi_cb);
        const drydock_rescue_callbacks_t rescue_cb = { .on_progress = NULL };
        drydock_rescue_init(&rescue_cb);
        build_home();
        build_detail();
        build_rescue();
        build_diag();
        build_sync();
        screen_show(s_scr_home);
        refresh_status();
    }

    xTaskCreate(refresh_task, "dry_ui", 4096, NULL, 4, NULL);
    return ESP_OK;
}
