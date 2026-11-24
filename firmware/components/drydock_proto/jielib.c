/* C port of the fm1_rescue.py algorithms (GPL-3.0, thefirmwarehub.com)
 * and jl-uboot-tool (MIT, kagaimiq). Verified bit-to-bit by the host test
 * companion/tests/test_fwsc.py against the Python reference.
 */
#include "jielib.h"

#include <string.h>

uint16_t fw_crc16(const uint8_t *data, size_t n, uint16_t crc)
{
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
        }
    }
    return crc;
}

void fw_enc(uint8_t *buf, size_t off, size_t size, uint16_t key)
{
    for (size_t i = off; i < off + size; i++) {
        buf[i] ^= key & 0xFF;
        key = ((key << 1) ^ ((key & 0x8000) ? 0x1021 : 0)) & 0xFFFF;
    }
}

void fw_sfc(uint8_t *buf, size_t size, uint16_t key)
{
    for (size_t i = 0; i < size; i += 32) {
        fw_enc(buf, i, size - i < 32 ? size - i : 32, key ^ (uint16_t)(i >> 2));
    }
}

/* "孟黎我爱你，玉林" in GB2312: the magic of the MengLi cipher. */
static const uint8_t s_magic[16] = { 0xC3, 0xCF, 0xC0, 0xE8, 0xCE, 0xD2, 0xB0, 0xAE,
                                     0xC4, 0xE3, 0xA3, 0xAC, 0xD3, 0xF1, 0xC1, 0xD6 };

void fw_mengli(uint8_t *buf, size_t len)
{
    uint16_t crc = fw_crc16((const uint8_t *)"\xFF\xFF", 2, 0xFFFF);
    for (size_t i = 0; i < len; i++) {
        crc = fw_crc16(s_magic + (i % sizeof(s_magic)), 1, crc);
        buf[i] ^= crc & 0xFF;
    }
}

void fw_cdb(uint8_t op, uint8_t sub, const uint8_t *args, size_t nargs, uint8_t out[16])
{
    out[0] = op;
    out[1] = sub;
    for (size_t i = 0; i < nargs && i < 14; i++) {
        out[2 + i] = args[i];
    }
    for (size_t i = 2 + nargs; i < 16; i++) {
        out[i] = 0xFF;
    }
}

int fw_chip_key(const uint8_t blob[34], uint16_t *out_key)
{
    const uint16_t crc = (uint16_t)(blob[32] | (blob[33] << 8));
    if (fw_crc16(blob, 32, 0) != crc) {
        return -1;
    }
    uint16_t s = 0;
    for (int i = 0; i < 16; i++) {
        s += blob[i];
    }
    s &= 0xFF;
    if (s <= 0x10) {
        s = 0x55;
    } else if (s >= 0xE0) {
        s = 0xAA;
    }
    uint16_t key = 0;
    for (int i = 0; i < 16; i++) {
        if ((uint8_t)(blob[16 + i] ^ blob[15 - i]) < (uint8_t)s) {
            key |= (uint16_t)(1u << i);
        }
    }
    *out_key = key;
    return 0;
}
