/* drydock_usb_msc_device — mode DEVICE_PC_SYNC : expose la partition FAT
 * interne (Wear Levelling) au PC en USB Mass Storage (TinyUSB MSC spiflash).
 *
 * Exclusion mutuelle stricte : en PC_SYNC le MCU ne monte PAS le VFS local.
 * Le handle WL vient de drydock_storage (drydock_fs_wl_init) ; ce composant
 * ne touche qu'au bloc, jamais au système de fichiers.
 */
#pragma once

#include "esp_err.h"
#include "wear_levelling.h"

/** Démarre l'USB Device MSC exposant le bloc WL. Renvoie ESP_OK si l'USB est prêt. */
esp_err_t drydock_msc_start(wl_handle_t wl_handle);

/** État courant : 1 si le stockage est exposé au PC (MOUNT_USB). */
int drydock_msc_is_exposed(void);

/** 1 si l'hôte USB est encore connecté (câble branché / énuméré). */
bool drydock_msc_host_connected(void);
