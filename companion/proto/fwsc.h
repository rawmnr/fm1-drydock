/* fwsc.h — parseur .fwsc (format Felucca / hub), portage C bit-à-bit de fm1_rescue.py.
 * Extrait la zone d'application flashable [APP_LO, APP_HI) du flash.bin embarqué.
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
    uint32_t flash_off;   /* offset du flash.bin dans l'image logique */
    uint32_t flash_size;
    /* pointe vers la zone d'app dans un tampon appartenant à l'appelant:
     * raw doit rester valide. Taille = FW_APP_HI - FW_APP_LO. */
    const uint8_t *app;
    uint32_t app_size;
    void *owned; /* tampon alloué par fwsc_parse (à libérer via fwsc_free) */
} fwsc_info_t;

/** Parse un .fwsc complet chargé en mémoire. Renvoie 0 si OK (tous les CRC validés). */
int fwsc_parse(const uint8_t *raw, size_t len, fwsc_info_t *out);

/** Libère le tampon interne alloué par fwsc_parse. */
void fwsc_free(fwsc_info_t *info);