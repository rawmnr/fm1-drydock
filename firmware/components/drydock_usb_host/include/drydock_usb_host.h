/* drydock_usb_host — cœur USB Host partagé (usb_host_lib).
 *
 * Installe la librairie, enregistre un client asynchrone et dispatche les
 * évènements d'appareil vers les drivers enregistrés (MIDI / Rescue). Un seul
 * appareil à la fois sur le port (le FM-1) : le premier driver dont match()
 * renvoie 1 est attaché, detach() est appelé quand il disparaît.
 *
 * API IDF v5.5 (usb_host_lib) : pas de pipe handle public. Après claim d'une
 * interface, on soumet des transferts dont bEndpointAddress désigne l'endpoint
 * et device_handle le device. Les callbacks de transfert s'exécutent dans le
 * contexte du client task (usb_host_client_handle_events) : ils donnent un
 * sémaphore et ne bloquent jamais.
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"

typedef struct drydock_usb_driver {
    const char *name;
    /** Renvoie 1 si ce driver prend l'appareil (VID/PID, classe...). */
    int (*match)(const usb_device_desc_t *dev, const usb_config_desc_t *cfg);
    /** Appelé (client task) une fois l'appareil ouvert : claim de l'interface. */
    esp_err_t (*attach)(usb_host_client_handle_t client, usb_device_handle_t dev,
                        const usb_config_desc_t *cfg);
    /** Appelé quand l'appareil disparaît (client task). */
    void (*detach)(void);
} drydock_usb_driver_t;

/** Installe usb_host_lib + client, démarre le client task.
 *  Les drivers sont scrutés dans l'ordre à chaque nouvel appareil. */
esp_err_t drydock_usb_host_start(const drydock_usb_driver_t *drivers, size_t n_drivers);

/** Arrête la librairie (appareil fermé, client désenregistré, lib désinstallée). */
esp_err_t drydock_usb_host_stop(void);

/** Handle du client (pour soumettre des transferts depuis les drivers). */
usb_host_client_handle_t drydock_usb_host_client(void);

/* ---- helpers de descripteur ---- */

/** Cherche la première interface de classe/sous-classe/protocole donnés
 *  (-1 = n'importe). Renvoie un pointeur dans cfg, NULL si absente. */
const usb_intf_desc_t *drydock_usb_find_interface(const usb_config_desc_t *cfg,
                                                  int class_, int subclass, int proto);

/** Cherche le premier endpoint IN (ou OUT si dir_out) de l'interface. */
const usb_ep_desc_t *drydock_usb_find_ep(const usb_intf_desc_t *iface, int dir_out);
