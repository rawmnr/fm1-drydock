/* drydock_usb_midi_host — USB Host MIDI engine + M-UPGRADE update protocol
 * of the FM-1 (thefirmwarehub.com).
 *
 * In normal mode the FM-1 enumerates as USB-MIDI (VID_4D4A&PID_4155). The
 * running firmware reads parts of the .fwsc packet and installs the update
 * loader, then that loader reads the complete image and writes it. Both
 * phases answer "cmd 0x30" read requests from the FM-1 on the logical image
 * (markers stripped).
 *
 * Asynchronous API: drydock_midi_flash() starts a task; the callbacks report
 * the state and progress. drydock_midi_send_enter_bootloader() sends the
 * UBOOT key (F0 22 24 35 7D F7) to switch to rescue mode.
 */
#pragma once

#include "drydock_usb_host.h"

typedef enum {
    DRYDOCK_MIDI_DETACHED,   /* no FM-1 MIDI on the port */
    DRYDOCK_MIDI_CONNECTED,  /* FM-1 present, identity read */
    DRYDOCK_MIDI_VERIFY,     /* step 1: the firmware reads the packet (staging loader) */
    DRYDOCK_MIDI_LOADER,     /* loader detected (ota-FM-1_9XX) */
    DRYDOCK_MIDI_WRITE,      /* step 2: the loader writes the image */
    DRYDOCK_MIDI_REBOOT,     /* waiting for the FM-1 to come back */
    DRYDOCK_MIDI_DONE,
    DRYDOCK_MIDI_ERROR,
} drydock_midi_state_t;

typedef struct {
    void (*on_state)(drydock_midi_state_t st, const char *detail);
    void (*on_progress)(unsigned served, uint32_t addr);
} drydock_midi_callbacks_t;

/** USB Host driver to register in drydock_usb_host_start(). */
const drydock_usb_driver_t *drydock_midi_driver(void);

/** Starts the engine (callbacks, driver). Call once, before the host. */
esp_err_t drydock_midi_init(const drydock_midi_callbacks_t *cb);

/** Starts a .fwsc flash (path on /drydock, e.g. "vessels/sloop.fwsc").
 *  Asynchronous: progress via the callbacks. Returns ESP_OK if the task
 *  starts. */
esp_err_t drydock_midi_flash(const char *path);

/** Sends the bootloader key (F0 22 24 35 7D F7): the FM-1 switches to
 *  UBOOT1.00 (MSC) for a rescue. */
esp_err_t drydock_midi_send_enter_bootloader(void);

/** Identity read at the last handshake (e.g. "FM-1_900", "ota-FM-1_900"). */
const char *drydock_midi_identity(void);

/** 1 if an FM-1 MIDI is attached. */
int drydock_midi_connected(void);

/** Last state + eventual error message. */
drydock_midi_state_t drydock_midi_state(void);
const char *drydock_midi_error(void);
