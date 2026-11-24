/* boot_mode.h — dual-role USB-C : HOST_FLASH (défaut) / DEVICE_PC_SYNC.
 * Le flag vit en RTC_NOINIT (survit à esp_restart, PAS à une coupure d'alimentation),
 * avec magic + CRC pour rejeter un flag corrompu.
 */
#pragma once

#include <stdint.h>

typedef enum {
    BOOT_MODE_HOST_FLASH = 0,   /* défaut : SD locale + USB Host (flash FM-1) */
    BOOT_MODE_PC_SYNC = 1,      /* TinyUSB MSC : expose la SD au PC */
    BOOT_MODE_COUNT
} boot_mode_t;

/** Mode qui sera appliqué au prochain boot (basculé via bootmode_switch). */
boot_mode_t boot_mode_next(void);

/** Lit le mode courant effectif (flag validé, sinon HOST_FLASH). */
boot_mode_t boot_mode_current(void);

/** Demande un redémarrage dans le mode donné (écrit le flag puis esp_restart). */
void boot_mode_switch(boot_mode_t mode);