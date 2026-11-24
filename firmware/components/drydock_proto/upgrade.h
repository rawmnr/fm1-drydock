/* upgrade.h — USB-MIDI M-UPGRADE update protocol of the FM-1, bit-exact C
 * port of the Web MIDI script from thefirmwarehub.com (GPL-3.0, zvenson).
 *
 * Step 1: the running firmware reads parts of the packet and installs the
 * update loader. Step 2: the loader reads the complete image and writes it.
 * In both steps the FM-1 sends "cmd 0x30" read requests on the logical image;
 * we answer them. Finish addresses: 0xE0000000 (step 1, the firmware has
 * installed the loader), 0xF0000000 (step 2, write complete).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/** Handshake query: the FM-1 replies with its identity (00 59 11). */
extern const uint8_t fw_up_hs_query[10];

/** UPGRADE key (F0 22 24 35 7F F7): installs the loader / starts the write. */
extern const uint8_t fw_up_upgrade[6];

/** UBOOT key (F0 22 24 35 7D F7): switches to UBOOT1.00 rescue mode (SCSI). */
extern const uint8_t fw_uboot_key[6];

/** Max size of a read requested by the FM-1 (512 B). */
#define FW_UP_MAXDATA 512u

/** Phase finish addresses (the FM-1 requests "success"). */
#define FW_UP_FINISH_VERIFY 0xE0000000u
#define FW_UP_FINISH_WRITE  0xF0000000u

/** Packs 7 bits (.fwsc packet served to the FM-1). Returns the written length. */
size_t fw_up_pack7(const uint8_t *in, size_t n, uint8_t *out);

/** Unpacks 7 bits (requests / identity received from the FM-1). */
size_t fw_up_unpack7(const uint8_t *in, size_t n, uint8_t *out);

/** Response to a 0x30 request: F0 .. F7 envelope + checksum.
 *  out must be able to hold 3 + (14 + len + 1) * 8/7 + 2. Returns the length. */
size_t fw_up_response(uint32_t addr, const uint8_t *data, size_t len, uint8_t fl,
                      uint8_t *out, size_t cap);

/** Parse a received 0x30 request (complete F0..F7 packet).
 *  Returns 1 for a valid request (addr/len/fl filled), 0 otherwise. */
int fw_up_parse_request(const uint8_t *pkt, size_t len, uint32_t *addr, size_t *dlen,
                        uint8_t *fl);

/** Parse a received identity (00 59 11) (complete F0..F7 packet).
 *  Returns 1 for a valid identity; out receives the text (e.g. "FM-1_900",
 *  "ota-FM-1_900"), max size out_sz-1 + NUL. */
int fw_up_parse_identity(const uint8_t *pkt, size_t len, char *out, size_t out_sz);
