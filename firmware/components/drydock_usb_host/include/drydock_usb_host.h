/* drydock_usb_host — shared USB Host core (usb_host_lib).
 *
 * Installs the library, registers an asynchronous client and dispatches
 * device events to the registered drivers (MIDI / Rescue). Only one device
 * at a time on the port (the FM-1): the first driver whose match() returns 1
 * is attached, detach() is called when it disappears.
 *
 * IDF v5.5 API (usb_host_lib): no public pipe handle. After claiming an
 * interface, we submit transfers whose bEndpointAddress designates the endpoint
 * and device_handle the device. Transfer callbacks run in the context of the
 * client task (usb_host_client_handle_events): they give a semaphore and never
 * block.
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"

typedef struct drydock_usb_driver {
    const char *name;
    /** Returns 1 if this driver takes the device (VID/PID, class...). */
    int (*match)(const usb_device_desc_t *dev, const usb_config_desc_t *cfg);
    /** Called (client task) once the device is open: claim the interface. */
    esp_err_t (*attach)(usb_host_client_handle_t client, usb_device_handle_t dev,
                        const usb_config_desc_t *cfg);
    /** Called when the device disappears (client task). */
    void (*detach)(void);
} drydock_usb_driver_t;

/** Installs usb_host_lib + client, starts the client task.
 *  The drivers are probed in order for each new device. */
esp_err_t drydock_usb_host_start(const drydock_usb_driver_t *drivers, size_t n_drivers);

/** Stops the library (device closed, client deregistered, lib uninstalled). */
esp_err_t drydock_usb_host_stop(void);

/** Client handle (to submit transfers from the drivers). */
usb_host_client_handle_t drydock_usb_host_client(void);

/* ---- descriptor helpers ---- */

/** Looks for the first interface of the given class/subclass/protocol
 *  (-1 = any). Returns a pointer into cfg, NULL if absent. */
const usb_intf_desc_t *drydock_usb_find_interface(const usb_config_desc_t *cfg,
                                                  int class_, int subclass, int proto);

/** Looks for the first IN endpoint (or OUT if dir_out) of the interface. */
const usb_ep_desc_t *drydock_usb_find_ep(const usb_intf_desc_t *iface, int dir_out);

/** Looks for the first interface that has both IN and OUT endpoints, of the
 *  given class (-1 = any). Windows only shows MI_00 of a composite MIDI
 *  (AudioControl, without endpoint): the class-only search misses the
 *  streaming interface. */
const usb_intf_desc_t *drydock_usb_find_data_iface(const usb_config_desc_t *cfg,
                                                   int class_, int subclass, int proto);
