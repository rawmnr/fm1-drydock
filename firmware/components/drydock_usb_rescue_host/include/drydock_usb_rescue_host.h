/* drydock_usb_rescue_host — USB Host rescue engine of the FM-1 (UBOOT1.00).
 *
 * The FM-1 in rescue mode enumerates as a USB disk (vendor "WL82", model
 * "UBOOT1.00") and speaks SCSI Bulk-Only Transport: 31-byte CBW, data,
 * 13-byte CSW. The JieLi bootloader v2 protocol (port of fm1_rescue.py) goes
 * through 16-byte CDBs (jielib.fw_cdb).
 *
 * Safety (protocol-rescue.md invariants) :
 *  - read-only by default (drydock_rescue_check) ;
 *  - flash only after explicit confirmation (drydock_rescue_flash
 *    with confirm=1) ;
 *  - the flash head [0x0000, 0x4000) is NEVER written ;
 *  - checks in order: flash chip, chip key, packet; otherwise stop without
 *    writing ;
 *  - every written sector is re-read (3 attempts), full final verification.
 */
#pragma once

#include "drydock_usb_host.h"

typedef struct {
    int found;              /* an UBOOT1.00 is present */
    char vendor[9];
    char model[17];
    int helper_running;     /* the loader helper already runs (resumed session) */
    int flash_ok;           /* flash chip recognized (0x8560 / 0xB360) */
    uint16_t chip_key;      /* chip key derived from the flash head */
    uint32_t head_crc_ok;   /* the head reads as JieLi (CRC) */
    uint32_t sectors_total; /* sectors of the app region */
    uint32_t sectors_diff;  /* sectors that differ from the packet */
} drydock_rescue_info_t;

typedef struct {
    void (*on_progress)(int done, int total, const char *what);
} drydock_rescue_callbacks_t;

/** USB Host driver to register in drydock_usb_host_start(). */
const drydock_usb_driver_t *drydock_rescue_driver(void);

/** Starts the engine (callbacks). Call once, before the host. */
esp_err_t drydock_rescue_init(const drydock_rescue_callbacks_t *cb);

/** Read-only inspection: identity, flash chip, chip key, comparison with
 *  the packet. Writes nothing. */
esp_err_t drydock_rescue_check(const char *fwsc_path, drydock_rescue_info_t *out);

/** Secure flash: only runs if confirm=1 (confirmed on screen).
 *  Writes only the sectors that differ, re-reads each sector,
 *  final verification, run_app. */
esp_err_t drydock_rescue_flash(const char *fwsc_path, int confirm);

/** 1 if an UBOOT1.00 is attached. */
int drydock_rescue_connected(void);

const char *drydock_rescue_error(void);
