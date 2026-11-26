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

typedef struct {
    uint8_t interface_number;
    uint8_t ep_in;
    uint8_t ep_out;
} drydock_usb_claim_t;

typedef struct drydock_usb_driver {
    const char *name;
    /** Returns 1 if this driver takes the device (VID/PID, class...). */
    int (*match)(const usb_device_desc_t *dev, const usb_config_desc_t *cfg);
    /** Called (client task) once the device is open: claim the interface. */
    esp_err_t (*attach)(usb_host_client_handle_t client, usb_device_handle_t dev,
                        const usb_config_desc_t *cfg, drydock_usb_claim_t *claim);
    /** Called when the device disappears (client task). */
    void (*detach)(void);
} drydock_usb_driver_t;

/** Starts the USB daemon and client. The driver table must outlive both tasks. */
esp_err_t drydock_usb_host_start(const drydock_usb_driver_t *const *drivers, size_t n_drivers);

/** Stops the library (device closed, client deregistered, lib uninstalled). */
esp_err_t drydock_usb_host_stop(void);

/** Submits on the active driver's claimed IN or OUT endpoint. */
esp_err_t drydock_usb_host_submit(const drydock_usb_driver_t *driver, usb_transfer_t *transfer,
                                  bool dir_out);

/* In-memory journal of the host lifecycle (enumerations, attach results,
 * disconnects). Survives FM-1 sessions where the serial console is gone
 * (the host owns the PHY): read back on the USB Diag screen. */
#define DRYDOCK_USB_JOURNAL_LINES 8
/** Returns the journal buffer (n filled lines, oldest first). */
const char *drydock_usb_journal(int *count);

/** Appends a formatted line to the journal (drivers, probe, engines). */
void drydock_usb_journal_add(const char *fmt, ...);

/* ---- descriptor helpers ---- */

/** Looks for the first interface of the given class/subclass/protocol
 *  (-1 = any). Returns a pointer into cfg, NULL if absent. */
const usb_intf_desc_t *drydock_usb_find_interface(const usb_config_desc_t *cfg,
                                                  int class_, int subclass, int proto);

/** Looks for the first IN endpoint (or OUT if dir_out) within this interface. */
const usb_ep_desc_t *drydock_usb_find_ep(const usb_config_desc_t *cfg,
                                          const usb_intf_desc_t *iface, bool dir_out);

/** Looks for the first interface that has both IN and OUT endpoints, of the
 *  given class (-1 = any). Windows only shows MI_00 of a composite MIDI
 *  (AudioControl, without endpoint): the class-only search misses the
 *  streaming interface. */
const usb_intf_desc_t *drydock_usb_find_data_iface(const usb_config_desc_t *cfg,
                                                   int class_, int subclass, int proto);
