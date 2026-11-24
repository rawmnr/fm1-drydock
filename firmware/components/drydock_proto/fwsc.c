/* C port of the fm1_rescue.py algorithms (GPL-3.0, thefirmwarehub.com).
 * Felucca .fwsc format: identity markers, ENC header, file list, flash.bin.
 */
#include "fwsc.h"

#include <stdlib.h>
#include <string.h>

#include "jielib.h"

static uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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

int fwsc_product_of(const uint8_t *raw, size_t len, char *out, size_t out_sz)
{
    if (len < 20u * 0x30u || out == NULL || out_sz == 0) {
        return -1;
    }
    size_t pk = 0;
    for (int i = 0; i < 20; i++) {
        const uint8_t m = raw[i * 0x30u + 0x2Fu];
        if (m != 0x7D && pk + 1 < out_sz) {
            out[pk++] = (char)((m - i - 1) & 0xFF);
        }
    }
    out[pk] = '\0';
    return 0;
}

int fwsc_logical_image(const uint8_t *raw, size_t len, uint8_t **out, size_t *out_len)
{
    if (len < 20u * 0x30u || out == NULL || out_len == NULL) {
        return -1;
    }
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
    *out = img;
    *out_len = img_len;
    return 0;
}

int fwsc_parse(const uint8_t *raw, size_t len, fwsc_info_t *out)
{
    if (len < 20u * 0x30u || raw == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    out->loader_present = (memmem_c(raw, len, FW_LOADER_MARK) != NULL);
    if (!out->loader_present) {
        return -1; /* not a hub packet (official V15 doesn't have one) */
    }

    if (fwsc_product_of(raw, len, out->product, sizeof(out->product)) != 0) {
        return -1;
    }

    /* logical image: strip the 20 markers */
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

    /* 0x40 encrypted ENC header */
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
    out->owned = img; /* transferred to the caller (fwsc_free), avoids a use-after-free */
    ret = 0;

done:
    if (ret != 0) {
        free(img);
    }
    return ret;
}

void fwsc_free(fwsc_info_t *info)
{
    if (info == NULL) {
        return;
    }
    if (info->owned) {
        free(info->owned);
        info->owned = NULL;
    }
}
