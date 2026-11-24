/* fwsc.h — .fwsc parser (Felucca / hub format), bit-exact C port of fm1_rescue.py.
 * Extracts the flashable application region [APP_LO, APP_HI) from the embedded
 * flash.bin, and exposes the logical image (markers stripped) served to the
 * FM-1 during an M-UPGRADE update.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define FW_APP_LO 0x4000u
#define FW_APP_HI 0x93000u
#define FW_LOADER_MARK "FELUCCA-LOADER-1"
#define FW_MAX_PRODUCT 24

typedef struct {
    char product[FW_MAX_PRODUCT + 1];
    int loader_present;
    int file_count;
    uint32_t flash_off;   /* offset of the flash.bin in the logical image */
    uint32_t flash_size;
    /* points at the app region in a buffer owned by the caller:
     * raw must remain valid. Size = FW_APP_HI - FW_APP_LO. */
    const uint8_t *app;
    uint32_t app_size;
    void *owned; /* buffer allocated by fwsc_parse (to free via fwsc_free) */
} fwsc_info_t;

/** Parse a complete .fwsc loaded in memory. Returns 0 if OK (all CRCs validated). */
int fwsc_parse(const uint8_t *raw, size_t len, fwsc_info_t *out);

/** Frees the internal buffer allocated by fwsc_parse. */
void fwsc_free(fwsc_info_t *info);

/** Product identity ("FM-1_904"): one marker byte after each of the first
 *  20 0x2F blocks. Works for any .fwsc (official V15 included).
 *  Returns 0 if OK, -1 if the packet is too short. */
int fwsc_product_of(const uint8_t *raw, size_t len, char *out, size_t out_sz);

/** Logical image: the .fwsc without the 20 marker bytes (the image the FM-1
 *  addresses during an M-UPGRADE update). Allocates *out (to be freed by the
 *  caller, free()). Returns 0 if OK. */
int fwsc_logical_image(const uint8_t *raw, size_t len, uint8_t **out, size_t *out_len);
