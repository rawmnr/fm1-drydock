/* fwsc.h — parseur .fwsc (format Felucca / hub), portage C bit-à-bit de fm1_rescue.py.
 * Extrait la zone d'application flashable [APP_LO, APP_HI) du flash.bin embarqué,
 * et expose l'image logique (marqueurs retirés) servie au FM-1 pendant un
 * update M-UPGRADE.
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

/** Identité produit ("FM-1_904") : un octet marqueur après chacun des 20
 *  premiers blocs 0x2F. Fonctionne pour tout .fwsc (V15 officiel compris).
 *  Renvoie 0 si OK, -1 si le paquet est trop court. */
int fwsc_product_of(const uint8_t *raw, size_t len, char *out, size_t out_sz);

/** Image logique : le .fwsc sans les 20 octets marqueurs (l'image que le FM-1
 *  adresse pendant un update M-UPGRADE). Alloue *out (à libérer par l'appelant,
 *  free()). Renvoie 0 si OK. */
int fwsc_logical_image(const uint8_t *raw, size_t len, uint8_t **out, size_t *out_len);
