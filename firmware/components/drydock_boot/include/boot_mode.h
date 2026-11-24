/* boot_mode.h — dual-role USB-C: HOST_FLASH (default) / DEVICE_PC_SYNC.
 * The flag lives in RTC_NOINIT (survives esp_restart, NOT a power cut),
 * with magic + CRC to reject a corrupted flag.
 */
#pragma once

#include <stdint.h>

typedef enum {
    BOOT_MODE_HOST_FLASH = 0,   /* default: local SD + USB Host (flash FM-1) */
    BOOT_MODE_PC_SYNC = 1,      /* TinyUSB MSC: expose the SD to the PC */
    BOOT_MODE_COUNT
} boot_mode_t;

/** Mode that will be applied at the next boot (switched via bootmode_switch). */
boot_mode_t boot_mode_next(void);

/** Reads the current effective mode (validated flag, otherwise HOST_FLASH). */
boot_mode_t boot_mode_current(void);

/** Requests a restart in the given mode (writes the flag then esp_restart). */
void boot_mode_switch(boot_mode_t mode);