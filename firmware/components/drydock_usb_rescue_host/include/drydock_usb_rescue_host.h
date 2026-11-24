/* drydock_usb_rescue_host — moteur USB Host rescue du FM-1 (UBOOT1.00).
 *
 * Le FM-1 en mode rescue s'énumère en disque USB (vendor "WL82", modèle
 * "UBOOT1.00") et parle SCSI Bulk-Only Transport : CBW 31 octets, données,
 * CSW 13 octets. Le protocole v2 du bootloader JieLi (portage de
 * fm1_rescue.py) passe par des CDB de 16 octets (jielib.fw_cdb).
 *
 * Sécurité (invariants protocol-rescue.md) :
 *  - lecture seule par défaut (drydock_rescue_check) ;
 *  - flash uniquement après confirmation explicite (drydock_rescue_flash
 *    avec confirm=1) ;
 *  - la tête du flash [0x0000, 0x4000) n'est JAMAIS écrite ;
 *  - vérifications dans l'ordre : chip flash, clé chip, paquet ; sinon arrêt
 *    sans écrire ;
 *  - chaque secteur écrit est relu (3 essais), vérification finale complète.
 */
#pragma once

#include "drydock_usb_host.h"

typedef struct {
    int found;              /* un UBOOT1.00 est présent */
    char vendor[9];
    char model[17];
    int helper_running;     /* le helper loader tourne déjà (session reprise) */
    int flash_ok;           /* chip flash reconnu (0x8560 / 0xB360) */
    uint16_t chip_key;      /* clé chip dérivée de la tête flash */
    uint32_t head_crc_ok;   /* la tête se lit comme du JieLi (CRC) */
    uint32_t sectors_total; /* secteurs de la zone app */
    uint32_t sectors_diff;  /* secteurs qui diffèrent du paquet */
} drydock_rescue_info_t;

typedef struct {
    void (*on_progress)(int done, int total, const char *what);
} drydock_rescue_callbacks_t;

/** Driver USB Host à enregistrer dans drydock_usb_host_start(). */
const drydock_usb_driver_t *drydock_rescue_driver(void);

/** Démarre le moteur (callbacks). À appeler une fois, avant le host. */
esp_err_t drydock_rescue_init(const drydock_rescue_callbacks_t *cb);

/** Inspection lecture seule : identité, chip flash, clé chip, comparaison
 *  avec le paquet. N'écrit rien. */
esp_err_t drydock_rescue_check(const char *fwsc_path, drydock_rescue_info_t *out);

/** Flash sécurisé : ne s'exécute que si confirm=1 (confirmé à l'écran).
 *  Écrit uniquement les secteurs qui diffèrent, relit chaque secteur,
 *  vérification finale, run_app. */
esp_err_t drydock_rescue_flash(const char *fwsc_path, int confirm);

/** 1 si un UBOOT1.00 est attaché. */
int drydock_rescue_connected(void);

const char *drydock_rescue_error(void);
