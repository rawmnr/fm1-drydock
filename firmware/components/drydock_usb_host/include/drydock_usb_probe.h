/* drydock_usb_probe — electrical presence probe of the USB-C port (D+/D-).
 *
 * The ESP32-S3's USB pads are GPIO19 (D-) / GPIO20 (D+). Before the USB host
 * takes the PHY, the pads can be sampled as plain GPIO inputs with only weak
 * internal pull-downs (~45 kOhm): a device's 1.5 kOhm pull-up still wins, so
 * the bus state tells *why* the FM-1 does not show up, which pure enumeration
 * never can (charge-only cable, FM-1 off or its battery too low, FM-1 left in
 * download mode, shorted cable...). Idea ported from fm1-flip (usb_bus.cpp).
 *
 * drydock_usb_bus_probe() must run BEFORE drydock_usb_host_start(): it hands
 * the pads from the USB-Serial-JTAG to the GPIO matrix for ~100 ms, then
 * leaves them undriven for the host. The USB-Serial-JTAG console goes silent
 * from there until the host is stopped — the host owns the PHY afterwards.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    DRYDOCK_BUS_EMPTY = 0,  /* nothing pulls D+ up: no device, FM-1 off,
                            * FM-1 battery too low, or charge-only cable */
    DRYDOCK_BUS_DEVICE,    /* steady full-speed pull-up on D+: a device is ready */
    DRYDOCK_BUS_UNSTABLE,  /* lines toggling: FM-1 left in download mode, or
                            * browning out / noisy connection */
    DRYDOCK_BUS_FAULT,     /* both lines high: cable short or wiring fault */
} drydock_bus_presence_t;

typedef struct {
    drydock_bus_presence_t presence;
    /* raw samples of each bus state (~20000 samples over ~100 ms), for logs */
    uint32_t se0;
    uint32_t j;      /* D+ high */
    uint32_t k;      /* D- high */
    uint32_t se1;    /* both high */
    uint32_t changes;
} drydock_bus_probe_t;

/** Samples the bus for ~100 ms. Runs before drydock_usb_host_start().
 *  Returns ESP_OK and fills *out; ESP_ERR_INVALID_ARG if out == NULL. */
esp_err_t drydock_usb_bus_probe(drydock_bus_probe_t *out);

/** Last probe result (all zeros / DRYDOCK_BUS_EMPTY if never probed). */
drydock_bus_probe_t drydock_usb_bus_last_probe(void);

/** Short classification name ("empty", "device", "unstable", "fault"). */
const char *drydock_bus_presence_name(drydock_bus_presence_t p);

/** Actionable hint for the USB Diag screen / logs. */
const char *drydock_bus_presence_hint(drydock_bus_presence_t p);
