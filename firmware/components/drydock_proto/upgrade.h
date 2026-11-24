/* upgrade.h — protocole de mise à jour USB-MIDI M-UPGRADE du FM-1, portage C
 * bit-à-bit du script Web MIDI de thefirmwarehub.com (GPL-3.0, zvenson).
 *
 * Étape 1 : le firmware en cours lit des parties du paquet et met en place le
 * loader de mise à jour. Étape 2 : le loader lit l'image complète et l'écrit.
 * Dans les deux étapes le FM-1 envoie des requêtes de lecture « cmd 0x30 » sur
 * l'image logique ; on y répond. Adresses de fin : 0xE0000000 (étape 1, le
 * firmware a mis en place le loader), 0xF0000000 (étape 2, écriture terminée).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/** Requête de poignée de main : le FM-1 répond par son identité (00 59 11). */
extern const uint8_t fw_up_hs_query[10];

/** Touche UPGRADE (F0 22 24 35 7F F7) : met en place le loader / lance l'écriture. */
extern const uint8_t fw_up_upgrade[6];

/** Touche UBOOT (F0 22 24 35 7D F7) : bascule en mode rescue UBOOT1.00 (SCSI). */
extern const uint8_t fw_uboot_key[6];

/** Taille max d'une lecture demandée par le FM-1 (512 B). */
#define FW_UP_MAXDATA 512u

/** Adresses de fin de phase (le FM-1 demande « success »). */
#define FW_UP_FINISH_VERIFY 0xE0000000u
#define FW_UP_FINISH_WRITE  0xF0000000u

/** Empile 7 bits (paquet .fwsc servie au FM-1). Renvoie la longueur écrite. */
size_t fw_up_pack7(const uint8_t *in, size_t n, uint8_t *out);

/** Dépaquette 7 bits (requêtes / identité reçues du FM-1). */
size_t fw_up_unpack7(const uint8_t *in, size_t n, uint8_t *out);

/** Réponse à une requête 0x30 : enveloppe F0 .. F7 + checksum.
 *  out doit pouvoir contenir 3 + (14 + len + 1) * 8/7 + 2. Renvoie la longueur. */
size_t fw_up_response(uint32_t addr, const uint8_t *data, size_t len, uint8_t fl,
                      uint8_t *out, size_t cap);

/** Parse une requête 0x30 reçue (paquet F0..F7 complet).
 *  Renvoie 1 si c'est une requête valide (addr/len/fl remplis), 0 sinon. */
int fw_up_parse_request(const uint8_t *pkt, size_t len, uint32_t *addr, size_t *dlen,
                        uint8_t *fl);

/** Parse une identité (00 59 11) reçue (paquet F0..F7 complet).
 *  Renvoie 1 si identité valide ; out reçoit le texte (ex. "FM-1_900",
 *  "ota-FM-1_900"), taille max out_sz-1 + NUL. */
int fw_up_parse_identity(const uint8_t *pkt, size_t len, char *out, size_t out_sz);
