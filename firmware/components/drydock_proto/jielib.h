/* jielib.h — JieLi ciphers (WL82 / UBOOT1.00) and SCSI CDB, bit-exact C port
 * of fm1_rescue.py (GPL-3.0) and jl-uboot-tool (MIT). Used by the rescue
 * host (drydock_usb_rescue_host) and by the .fwsc parser (fwsc.c).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/** CRC-16/XMODEM (poly 0x1021): .fwsc packets, flash records, bootloader. */
uint16_t fw_crc16(const uint8_t *data, size_t n, uint16_t crc);

/** ENC stream cipher (xor + LFSR poly 0x1021), in place. */
void fw_enc(uint8_t *buf, size_t off, size_t size, uint16_t key);

/** SFC: ENC in 32-byte blocks, key xored with (offset >> 2). */
void fw_sfc(uint8_t *buf, size_t size, uint16_t key);

/** MengLi CRC cipher (ROM RAM memory), its own inverse. */
void fw_mengli(uint8_t *buf, size_t len);

/** 16-byte SCSI CDB: op, sub, args (<= 13), padded with 0xFF. */
void fw_cdb(uint8_t op, uint8_t sub, const uint8_t *args, size_t nargs, uint8_t out[16]);

/** Chip key from the 34-byte blob (32 + CRC16 LE) read at the flash head.
 *  Returns 0 if the blob is valid (CRC OK), -1 otherwise. */
int fw_chip_key(const uint8_t blob[34], uint16_t *out_key);
