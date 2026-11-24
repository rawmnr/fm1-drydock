/* drydock_usb_msc_device — DEVICE_PC_SYNC mode: exposes the internal FAT
 * partition (Wear Levelling) to the PC as USB Mass Storage (TinyUSB MSC
 * spiflash).
 *
 * Strict mutual exclusion: in PC_SYNC the MCU does NOT mount the local VFS.
 * The WL handle comes from drydock_storage (drydock_fs_wl_init); this
 * component only touches the block, never the filesystem.
 */
#pragma once

#include "esp_err.h"
#include "wear_levelling.h"

/** Starts the USB Device MSC exposing the WL block. Returns ESP_OK if the USB is ready. */
esp_err_t drydock_msc_start(wl_handle_t wl_handle);

/** Current state: 1 if the storage is exposed to the PC (MOUNT_USB). */
int drydock_msc_is_exposed(void);

/** 1 if the USB host is still connected (cable plugged in / enumerated). */
bool drydock_msc_host_connected(void);
