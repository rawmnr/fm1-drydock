/* drydock_ui — interface LVGL 9 de la cartouche (368x448 tactile).
 *
 * Écrans : accueil (liste des vessels + actions rapides), détail + flash
 * (vérif SHA-256, barre de progression, "Flash Stock V15 first"),
 * Rescue & Stock (retour V15, bootloader UBOOT1.00), PC Sync (état MSC +
 * Done & Reboot), USB Diag (VBUS, host lib, VID:PID), et barre de statut
 * (DRYDOCK, batterie AXP2101, état USB, espace libre).
 *
 * À appeler après bsp_display_start() : construit les écrans et démarre la
 * tâche de rafraîchissement. En mode DEVICE_PC_SYNC le VFS n'est pas monté :
 * seul l'écran PC Sync est affiché (pas de lecture de manifest).
 */
#pragma once

#include "esp_err.h"

/** Construit l'UI et démarre le rafraîchissement périodique. */
esp_err_t drydock_ui_start(void);
