/* Portage C du protocole M-UPGRADE (thefirmwarehub.com, GPL-3.0, zvenson).
 * Vérifié bit-à-bit par le test hôte companion/tests/test_fwsc.py.
 */
#include "upgrade.h"

#include <string.h>

const uint8_t fw_up_hs_query[10] = { 0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7 };
const uint8_t fw_up_upgrade[6] = { 0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7 };
const uint8_t fw_uboot_key[6] = { 0xF0, 0x22, 0x24, 0x35, 0x7D, 0xF7 };

size_t fw_up_pack7(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 0;
    uint32_t acc = 0;
    int nb = 0;
    for (size_t i = 0; i < n; i++) {
        acc |= (uint32_t)in[i] << nb;
        nb += 8;
        while (nb >= 7) {
            out[o++] = acc & 0x7F;
            acc >>= 7;
            nb -= 7;
        }
    }
    if (nb) {
        out[o++] = acc & 0x7F;
    }
    return o;
}

size_t fw_up_unpack7(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 0;
    uint32_t acc = 0;
    int nb = 0;
    for (size_t i = 0; i < n; i++) {
        acc |= (uint32_t)(in[i] & 0x7F) << nb;
        nb += 7;
        while (nb >= 8) {
            out[o++] = acc & 0xFF;
            acc >>= 8;
            nb -= 8;
        }
    }
    return o;
}

size_t fw_up_response(uint32_t addr, const uint8_t *data, size_t len, uint8_t fl,
                      uint8_t *out, size_t cap)
{
    /* corps : 00 59 30 (len+8) (len+8)>>8 00 fl addr(4 LE) len(2 LE) 00 data */
    uint8_t body[14 + FW_UP_MAXDATA + 1];
    if (len > FW_UP_MAXDATA) {
        return 0;
    }
    const size_t body_len = 14 + len;
    body[0] = 0x00;
    body[1] = 0x59;
    body[2] = 0x30;
    body[3] = (len + 8) & 0xFF;
    body[4] = ((len + 8) >> 8) & 0xFF;
    body[5] = 0;
    body[6] = fl;
    body[7] = addr & 0xFF;
    body[8] = (addr >> 8) & 0xFF;
    body[9] = (addr >> 16) & 0xFF;
    body[10] = (addr >> 24) & 0xFF;
    body[11] = len & 0xFF;
    body[12] = (len >> 8) & 0xFF;
    body[13] = 0;
    memcpy(body + 14, data, len);

    uint8_t s = 0;
    for (size_t i = 6; i < body_len; i++) {
        s = (uint8_t)(s + body[i]);
    }
    body[body_len] = (uint8_t)~s;

    size_t o = 0;
    if (cap >= 2) {
        out[o++] = 0xF0;
    }
    o += fw_up_pack7(body, body_len + 1, out + o);
    if (cap > o) {
        out[o++] = 0xF7;
    }
    return o;
}

int fw_up_parse_request(const uint8_t *pkt, size_t len, uint32_t *addr, size_t *dlen,
                        uint8_t *fl)
{
    if (len < 4 || pkt[0] != 0xF0 || pkt[len - 1] != 0xF7) {
        return 0;
    }
    uint8_t u[32];
    const size_t un = fw_up_unpack7(pkt + 1, len - 2, u);
    if (un != 15 || u[0] != 0 || u[1] != 0x59 || u[2] != 0x30) {
        return 0;
    }
    uint8_t s = 0;
    for (int i = 6; i < 14; i++) {
        s = (uint8_t)(s + u[i]);
    }
    if ((uint8_t)~s != u[14]) {
        return 0;
    }
    *fl = u[6];
    *addr = (uint32_t)u[7] | ((uint32_t)u[8] << 8) | ((uint32_t)u[9] << 16) |
            ((uint32_t)u[10] << 24);
    *dlen = (size_t)u[11] | ((size_t)u[12] << 8) | ((size_t)u[13] << 16);
    return 1;
}

int fw_up_parse_identity(const uint8_t *pkt, size_t len, char *out, size_t out_sz)
{
    if (len < 4 || pkt[0] != 0xF0 || pkt[len - 1] != 0xF7) {
        return 0;
    }
    uint8_t d[64];
    const size_t dn = fw_up_unpack7(pkt + 1, len - 2, d);
    if (dn != 34 || d[0] != 0 || d[1] != 0x59 || d[2] != 0x11) {
        return 0;
    }
    /* texte d[6..32], NUL-terminé, sans les 0 de fin */
    size_t t = 0;
    while (t < 27 && d[6 + t] != 0) {
        t++;
    }
    size_t w = t < out_sz - 1 ? t : out_sz - 1;
    memcpy(out, d + 6, w);
    out[w] = '\0';
    return 1;
}
