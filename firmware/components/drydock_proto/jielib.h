/* jielib.h — ciphers JieLi (WL82 / UBOOT1.00) et CDB SCSI, portage C bit-à-bit
 * de fm1_rescue.py (GPL-3.0) et jl-uboot-tool (MIT). Utilisé par le host
 * rescue (drydock_usb_rescue_host) et par le parseur .fwsc (fwsc.c).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/** CRC-16/XMODEM (poly 0x1021) : paquets .fwsc, records flash, bootloader. */
uint16_t fw_crc16(const uint8_t *data, size_t n, uint16_t crc);

/** Chiffre de flux ENC (xor + LFSR poly 0x1021), en place. */
void fw_enc(uint8_t *buf, size_t off, size_t size, uint16_t key);

/** SFC : ENC par blocs de 32 octets, clé xorée avec (offset >> 2). */
void fw_sfc(uint8_t *buf, size_t size, uint16_t key);

/** Chiffre CRC MengLi (mémoire RAM du ROM), sa propre inverse. */
void fw_mengli(uint8_t *buf, size_t len);

/** CDB SCSI 16 octets : op, sub, args (<= 13), complété de 0xFF. */
void fw_cdb(uint8_t op, uint8_t sub, const uint8_t *args, size_t nargs, uint8_t out[16]);

/** Clé chip depuis le blob 34 octets (32 + CRC16 LE) lu en tête flash.
 *  Renvoie 0 si le blob est valide (CRC OK), -1 sinon. */
int fw_chip_key(const uint8_t blob[34], uint16_t *out_key);
