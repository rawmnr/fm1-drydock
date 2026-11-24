/* drydock_usb_midi_host — moteur USB Host MIDI + protocole de mise à jour
 * M-UPGRADE du FM-1 (thefirmwarehub.com).
 *
 * En mode normal le FM-1 s'énumère en USB-MIDI (VID_4D4A&PID_4155). Le firmware
 * en cours d'exécution lit des parties du paquet .fwsc et met en place le
 * loader de mise à jour, puis ce loader lit l'image complète et l'écrit.
 * Les deux phases répondent à des requêtes de lecture « cmd 0x30 » du FM-1 sur
 * l'image logique (marqueurs retirés).
 *
 * API asynchrone : drydock_midi_flash() démarre une tâche ; les callbacks
 * rendent l'état et la progression. drydock_midi_send_enter_bootloader()
 * envoie la touche UBOOT (F0 22 24 35 7D F7) pour basculer en mode rescue.
 */
#pragma once

#include "drydock_usb_host.h"

typedef enum {
    DRYDOCK_MIDI_DETACHED,   /* pas de FM-1 MIDI sur le port */
    DRYDOCK_MIDI_CONNECTED,  /* FM-1 présent, identité lue */
    DRYDOCK_MIDI_VERIFY,     /* étape 1 : le firmware lit le paquet (staging loader) */
    DRYDOCK_MIDI_LOADER,     /* loader détecté (ota-FM-1_9XX) */
    DRYDOCK_MIDI_WRITE,      /* étape 2 : le loader écrit l'image */
    DRYDOCK_MIDI_REBOOT,     /* attente du retour du FM-1 */
    DRYDOCK_MIDI_DONE,
    DRYDOCK_MIDI_ERROR,
} drydock_midi_state_t;

typedef struct {
    void (*on_state)(drydock_midi_state_t st, const char *detail);
    void (*on_progress)(unsigned served, uint32_t addr);
} drydock_midi_callbacks_t;

/** Driver USB Host à enregistrer dans drydock_usb_host_start(). */
const drydock_usb_driver_t *drydock_midi_driver(void);

/** Démarre le moteur (callbacks, driver). À appeler une fois, avant le host. */
esp_err_t drydock_midi_init(const drydock_midi_callbacks_t *cb);

/** Lance un flash .fwsc (chemin sur /drydock, ex. "vessels/sloop.fwsc").
 *  Asynchrone : progression via les callbacks. Renvoie ESP_OK si la tâche
 *  démarre. */
esp_err_t drydock_midi_flash(const char *path);

/** Envoie la touche bootloader (F0 22 24 35 7D F7) : le FM-1 passe en
 *  UBOOT1.00 (MSC) pour un rescue. */
esp_err_t drydock_midi_send_enter_bootloader(void);

/** Identité lue au dernier handshake (ex. "FM-1_900", "ota-FM-1_900"). */
const char *drydock_midi_identity(void);

/** 1 si un FM-1 MIDI est attaché. */
int drydock_midi_connected(void);

/** Dernier état + message d'erreur éventuel. */
drydock_midi_state_t drydock_midi_state(void);
const char *drydock_midi_error(void);
