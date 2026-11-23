/* Portage C des algorithmes de fm1_rescue.py (GPL-3.0, thefirmwarehub.com).
 * Format .fwsc Felucca : marqueurs d'identité, en-tête ENC, liste de fichiers, flash.bin.
 */
#include "fwsc.h"

#include <string.h>

static uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

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

static const uint8_t *memmem_c(const uint8_t *h, size_t n, const char *needle)
{
    const size_t m = strlen(needle);
    if (m == 0 || m > n) {
        return NULL;
    }
    for (size_t i = 0; i + m <= n; i++) {
        if (memcmp(h + i, needle, m) == 0) {
            return h + i;
        }
    }
    return NULL;
}

int fwsc_parse(const uint8_t *raw, size_t len, fwsc_info_t *out)
{
    if (len < 20u * 0x30u || raw == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    out->loader_present = (memmem_c(raw, len, FW_LOADER_MARK) != NULL);
    if (!out->loader_present) {
        return -1; /* pas un paquet hub (V15 officiel n'en a pas) */
    }

    /* identité produit : un octet marqueur après chacun des 20 premiers blocs 0x2F */
    size_t pk = 0;
    for (int i = 0; i < 20; i++) {
        const uint8_t m = raw[i * 0x30u + 0x2Fu];
        if (m != 0x7D && pk < FW_MAX_PRODUCT) {
            out->product[pk++] = (char)((m - i - 1) & 0xFF);
        }
    }
    out->product[pk] = '\0';

    /* image logique : on retire les 20 marqueurs */
    const size_t img_len = len - 20;
    uint8_t *img = calloc(img_len, 1);
    if (img == NULL) {
        return -1;
    }
    size_t d = 0;
    for (int i = 0; i < 20; i++) {
        memcpy(img + d, raw + i * 0x30u, 0x2Fu);
        d += 0x2Fu;
    }
    memcpy(img + d, raw + 20u * 0x30u, len - 20u * 0x30u);

    /* en-tête 0x40 chiffré ENC */
    uint8_t hdr[0x40];
    memcpy(hdr, img, sizeof(hdr));
    fw_enc(hdr, 0, sizeof(hdr), 0xFFFF);
    const uint16_t h0 = rd16le(hdr);
    const uint16_t h2 = rd16le(hdr + 2);
    const uint16_t count = rd16le(hdr + 8);

    int ret = -1;
    if (fw_crc16(hdr + 2, 0x40 - 2, 0) != h0 || count < 1 || count > 11) {
        goto done;
    }
    const size_t lst_size = (size_t)count * 0x50u;
    if (img_len < 0x40u + lst_size) {
        goto done;
    }
    if (fw_crc16(img + 0x40, lst_size, 0) != h2) {
        goto done;
    }

    uint32_t flash_off = 0, flash_size = 0;
    int found = 0;
    for (int j = 0; j < count; j++) {
        const uint8_t *e0 = img + 0x40u + (size_t)j * 0x50u;
        uint8_t e[0x50];
        memcpy(e, e0, sizeof(e));
        fw_enc(e, 0, sizeof(e), 0xFFFF);
        const uint16_t typ = rd16le(e);
        const uint16_t dcrc = rd16le(e + 4);
        const uint32_t off = rd32le(e + 8);
        const uint32_t size = rd32le(e + 12);
        if (off > img_len || size > img_len - off) {
            goto done;
        }
        if (fw_crc16(img + off, size, 0) != dcrc) {
            goto done;
        }
        if (typ == 0) {
            flash_off = off;
            flash_size = size;
            found = 1;
        }
    }
    if (!found || flash_size < FW_APP_HI) {
        goto done;
    }

    out->file_count = count;
    out->flash_off = flash_off;
    out->flash_size = flash_size;
    out->app = img + flash_off + FW_APP_LO;
    out->app_size = FW_APP_HI - FW_APP_LO;
    ret = 0;

done:
    free(img);
    return ret;
}