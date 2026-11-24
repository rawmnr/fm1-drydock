/* drydock_ui — LVGL 9 interface of the cartridge (368x448 touch).
 *
 * Screens: home (vessel list + quick actions), detail + flash
 * (SHA-256 check, progress bar, "Flash Stock V15 first"),
 * Rescue & Stock (back to V15, UBOOT1.00 bootloader), PC Sync (MSC state +
 * Done & Reboot), USB Diag (VBUS, host lib, VID:PID), and a status bar
 * (DRYDOCK, AXP2101 battery, USB state, free space).
 *
 * Call with the LVGL lock held after display setup. Builds the screens and starts
 * the refresh task. In DEVICE_PC_SYNC mode only the PC Sync screen is shown
 * because the VFS isn't mounted.
 */
#pragma once

#include "esp_err.h"

/** Builds the UI and starts periodic refresh. */
esp_err_t drydock_ui_start(void);
