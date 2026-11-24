/* USB Host MIDI engine: FM-1 detection (VID_4D4A&PID_4155), SysEx MIDI
 * transport (USB MIDI event packets) and M-UPGRADE update protocol.
 *
 * The USB-MIDI transport splits each SysEx into 4-byte packets
 * [CIN<<4 | cable, d0, d1, d2]: CIN 0x4 start, 0x5 continue, 0x7 end.
 * The FM-1 sends its 0x30 requests as SysEx (framed F0..F7, see upgrade.c).
 */
#include "drydock_usb_midi_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fwsc.h"
#include "upgrade.h"

#define MIDI_VID 0x4D4A
#define MIDI_PID 0x4155

#define MIDI_CABLE 0
#define CIN_SYSEX_START 0x4
#define CIN_SYSEX_CONT 0x5
#define CIN_SYSEX_END 0x7

#define XFER_BUF_SIZE 512 /* >= MIDI MPS (64) and 2× packets */
#define HS_TIMEOUT_MS 1200
#define HS_TRIES 3

static const char *TAG = "dry_midi";

typedef struct {
    usb_device_handle_t dev;
    uint8_t ep_out;
    uint8_t ep_in;
} midi_dev_t;

static midi_dev_t s_dev;
static volatile int s_attached;
static char s_identity[32];
static char s_error[96];
static volatile drydock_midi_state_t s_state = DRYDOCK_MIDI_DETACHED;
static drydock_midi_callbacks_t s_cb;
static char s_flash_path[128];
static volatile int s_flash_pending;
static volatile int s_abort;

static SemaphoreHandle_t s_xfer_done;
static usb_transfer_t *s_xfer;
static volatile int s_xfer_status;

static const char *state_name(drydock_midi_state_t st)
{
    static const char *names[] = {
        "detached", "connected", "verify", "loader", "write", "reboot", "done", "error",
    };
    return st <= DRYDOCK_MIDI_ERROR ? names[st] : "?";
}

static void set_state(drydock_midi_state_t st, const char *detail)
{
    s_state = st;
    if (st == DRYDOCK_MIDI_ERROR && detail != NULL) {
        snprintf(s_error, sizeof(s_error), "%s", detail);
    }
    ESP_LOGI(TAG, "state=%s %s", state_name(st), detail ? detail : "");
    if (s_cb.on_state) {
        s_cb.on_state(st, detail);
    }
}

/* ---- USB-MIDI transport ---- */

static void xfer_cb(usb_transfer_t *xfer)
{
    s_xfer_status = xfer->status;
    xSemaphoreGive(s_xfer_done);
}

static esp_err_t xfer_wait(int timeout_ms)
{
    if (xSemaphoreTake(s_xfer_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_xfer_status != USB_TRANSFER_STATUS_COMPLETED) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

/* Splits a SysEx (with F0..F7) into 4-byte MIDI packets and sends it. */
static esp_err_t midi_send_sysex(const uint8_t *sysex, size_t len)
{
    if (!s_attached || s_xfer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t out[XFER_BUF_SIZE];
    size_t o = 0;
    size_t i = 0;
    while (i < len) {
        const size_t chunk = len - i > 3 ? 3 : len - i;
        uint8_t cin;
        if (i == 0) {
            cin = CIN_SYSEX_START;
        } else if (i + chunk == len) {
            cin = CIN_SYSEX_END;
        } else {
            cin = CIN_SYSEX_CONT;
        }
        out[o++] = (uint8_t)((cin << 4) | MIDI_CABLE);
        out[o++] = sysex[i];
        out[o++] = i + 1 < len ? sysex[i + 1] : 0;
        out[o++] = i + 2 < len ? sysex[i + 2] : 0;
        i += chunk;
    }
    s_xfer->device_handle = s_dev.dev;
    s_xfer->bEndpointAddress = s_dev.ep_out;
    s_xfer->num_bytes = (int)o;
    s_xfer->timeout_ms = 1000;
    memcpy(s_xfer->data_buffer, out, o);
    ESP_RETURN_ON_ERROR(usb_host_transfer_submit(s_xfer), TAG, "submit OUT");
    return xfer_wait(2000);
}

/* Receives a complete SysEx (F0..F7): accumulates the 0x4/0x5/0x7 packets.
 * Returns 0 once a SysEx is reassembled (out/out_len), -1 otherwise (timeout). */
static int midi_recv_sysex(uint8_t *out, size_t out_cap, size_t *out_len, int timeout_ms)
{
    uint8_t acc[512];
    size_t n = 0;
    const uint32_t end = (uint32_t)(xTaskGetTickCount()) + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < end) {
        if (!s_attached || s_abort) {
            return -1;
        }
        s_xfer->device_handle = s_dev.dev;
        s_xfer->bEndpointAddress = s_dev.ep_in;
        s_xfer->num_bytes = XFER_BUF_SIZE;
        s_xfer->timeout_ms = 1000;
        if (usb_host_transfer_submit(s_xfer) != ESP_OK) {
            return -1;
        }
        if (xfer_wait(1500) != ESP_OK) {
            return -1; /* erreur ou timeout : on abandonne */
        }
        const int got = s_xfer->actual_num_bytes;
        for (int i = 0; i + 3 < got && n < sizeof(acc); i += 4) {
            const uint8_t hdr = s_xfer->data_buffer[i];
            const uint8_t cin = hdr >> 4;
            if (cin != CIN_SYSEX_START && cin != CIN_SYSEX_CONT && cin != CIN_SYSEX_END) {
                continue; /* non-SysEx message: ignored */
            }
            const size_t take = (size_t)(got - i - 1) < 3 ? (size_t)(got - i - 1) : 3;
            for (size_t k = 0; k < take && n < sizeof(acc); k++) {
                acc[n++] = s_xfer->data_buffer[i + 1 + k];
            }
            if (cin == CIN_SYSEX_END) {
                if (n >= 2 && acc[0] == 0xF0 && acc[n - 1] == 0xF7 && n <= out_cap) {
                    memcpy(out, acc, n);
                    *out_len = n;
                    return 0;
                }
                n = 0; /* truncated or badly delimited SysEx: start over */
            }
        }
    }
    return -1;
}

/* ---- M-UPGRADE protocol ---- */

static int wait_identity(char *out, size_t out_sz, int timeout_ms)
{
    const uint32_t end = (uint32_t)xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while ((uint32_t)xTaskGetTickCount() < end) {
        uint8_t pkt[512];
        size_t n = 0;
        if (midi_recv_sysex(pkt, sizeof(pkt), &n, 1500) == 0 &&
            fw_up_parse_identity(pkt, n, out, out_sz)) {
            return 0;
        }
    }
    return -1;
}

static int handshake(char *out, size_t out_sz)
{
    for (int t = 0; t < HS_TRIES; t++) {
        if (midi_send_sysex(fw_up_hs_query, sizeof(fw_up_hs_query)) != ESP_OK) {
            continue;
        }
        if (wait_identity(out, out_sz, HS_TIMEOUT_MS) == 0) {
            return 0;
        }
    }
    return -1;
}

/* Serves the 0x30 read requests until the FM-1 asks for the finish address.
 * Returns 1 if done, 0 otherwise (timeout/lost). */
static int serve(const uint8_t *image, size_t image_len, uint32_t finish, int idle_ms,
                 int step)
{
    int served = 0;
    uint32_t last = (uint32_t)xTaskGetTickCount();
    while (1) {
        if (s_abort || !s_attached) {
            return 0;
        }
        uint8_t pkt[512];
        size_t n = 0;
        if (midi_recv_sysex(pkt, sizeof(pkt), &n, 1000) != 0) {
            if ((uint32_t)xTaskGetTickCount() - last > pdMS_TO_TICKS(idle_ms)) {
                return 0; /* idle too long */
            }
            continue;
        }
        last = (uint32_t)xTaskGetTickCount();
        uint32_t addr = 0;
        size_t dlen = 0;
        uint8_t fl = 0;
        if (!fw_up_parse_request(pkt, n, &addr, &dlen, &fl)) {
            continue;
        }
        if (addr == 0xE0000000u || addr == 0xF0000000u) {
            uint8_t rsp[64];
            const size_t rn = fw_up_response(addr, (const uint8_t *)"success", 7, fl,
                                             rsp, sizeof(rsp));
            if (rn && midi_send_sysex(rsp, rn) == ESP_OK && addr == finish) {
                return 1;
            }
            continue;
        }
        if (dlen > FW_UP_MAXDATA || addr > image_len || dlen > image_len - addr) {
            set_state(DRYDOCK_MIDI_ERROR, "request out of image");
            return 0;
        }
        uint8_t rsp[FW_UP_MAXDATA + 64];
        const size_t rn = fw_up_response(addr, image + addr, dlen, fl, rsp, sizeof(rsp));
        if (rn == 0) {
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(10)); /* pacing identical to the hub (sleep(10)) */
        if (midi_send_sysex(rsp, rn) != ESP_OK) {
            return 0;
        }
        served++;
        if (s_cb.on_progress) {
            s_cb.on_progress((unsigned)served, addr);
        }
        if (step == 0) {
            ESP_LOGI(TAG, "verify: %d requests, addr=0x%08X", served, addr);
        } else {
            ESP_LOGI(TAG, "write: %d requests, addr=0x%08X", served, addr);
        }
    }
}

/* ---- USB Host driver ---- */

static int match_fm1(const usb_device_desc_t *dev, const usb_config_desc_t *cfg)
{
    (void)cfg;
    return dev->idVendor == MIDI_VID && dev->idProduct == MIDI_PID;
}

static esp_err_t attach_fm1(usb_host_client_handle_t client, usb_device_handle_t dev,
                            const usb_config_desc_t *cfg)
{
    (void)client;
    /* MIDI streaming interface: class Audio (0x01) subclass MIDI (0x03),
     * otherwise any interface with IN/OUT endpoints (the FM-1 exposes
     * MI_00 AudioControl without endpoint; Windows only shows that one). */
    const usb_intf_desc_t *iface = drydock_usb_find_data_iface(cfg, 0x01, 0x03, -1);
    if (iface == NULL) {
        iface = drydock_usb_find_data_iface(cfg, -1, -1, -1);
    }
    if (iface == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(
        usb_host_interface_claim(client, dev, iface->bInterfaceNumber, iface->bAlternateSetting),
        TAG, "claim iface MIDI");
    const usb_ep_desc_t *ep_out = drydock_usb_find_ep(iface, 1);
    const usb_ep_desc_t *ep_in = drydock_usb_find_ep(iface, 0);
    if (ep_out == NULL || ep_in == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_dev.dev = dev;
    s_dev.ep_out = ep_out->bEndpointAddress;
    s_dev.ep_in = ep_in->bEndpointAddress;
    s_attached = 1;
    s_abort = 0;
    return ESP_OK;
}

static void detach_fm1(void)
{
    s_attached = 0;
    s_dev.dev = NULL;
    s_identity[0] = '\0';
}

static const drydock_usb_driver_t s_driver = {
    .name = "fm1-midi",
    .match = match_fm1,
    .attach = attach_fm1,
    .detach = detach_fm1,
};

const drydock_usb_driver_t *drydock_midi_driver(void)
{
    return &s_driver;
}

/* ---- engine task ---- */

static void midi_task(void *arg)
{
    (void)arg;
    while (1) {
        if (s_abort) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!s_attached) {
            if (s_state != DRYDOCK_MIDI_DETACHED) {
                set_state(DRYDOCK_MIDI_DETACHED, NULL);
            }
            s_flash_pending = 0;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (s_flash_pending) {
            s_flash_pending = 0;
            /* handshake + identity */
            char id[32];
            if (handshake(id, sizeof(id)) != 0) {
                set_state(DRYDOCK_MIDI_ERROR, "no FM-1 response (handshake)");
                continue;
            }
            snprintf(s_identity, sizeof(s_identity), "%s", id);
            ESP_LOGI(TAG, "identity: %s", id);

            /* load the packet */
            char path[sizeof(s_flash_path) + 32];
            snprintf(path, sizeof(path), "/drydock/%s", s_flash_path);
            FILE *f = fopen(path, "rb");
            if (f == NULL) {
                set_state(DRYDOCK_MIDI_ERROR, "packet not found");
                continue;
            }
            fseek(f, 0, SEEK_END);
            const long flen = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (flen <= 0) {
                fclose(f);
                set_state(DRYDOCK_MIDI_ERROR, "empty packet");
                continue;
            }
            uint8_t *raw = heap_caps_malloc((size_t)flen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (raw == NULL || fread(raw, 1, (size_t)flen, f) != (size_t)flen) {
                fclose(f);
                if (raw) {
                    free(raw);
                }
                set_state(DRYDOCK_MIDI_ERROR, "packet read failed");
                continue;
            }
            fclose(f);

            uint8_t *image = NULL;
            size_t image_len = 0;
            char product[FW_MAX_PRODUCT + 1];
            if (fwsc_logical_image(raw, (size_t)flen, &image, &image_len) != 0 ||
                fwsc_product_of(raw, (size_t)flen, product, sizeof(product)) != 0) {
                free(raw);
                set_state(DRYDOCK_MIDI_ERROR, "invalid packet");
                continue;
            }
            free(raw); /* the logical image is an independent copy */
            ESP_LOGI(TAG, "logical image %u bytes, product %s", (unsigned)image_len, product);

            /* model: the packet is for an FM-1 (FM-1_xxx) */
            char model[24];
            {
                const char *us = strchr(product, '_');
                const size_t ml = us ? (size_t)(us - product) : strlen(product);
                const size_t cp = ml < sizeof(model) - 1 ? ml : sizeof(model) - 1;
                memcpy(model, product, cp);
                model[cp] = '\0';
            }
            if (strcmp(model, "FM-1") != 0) {
                free(image);
                set_state(DRYDOCK_MIDI_ERROR, "packet not for an FM-1");
                continue;
            }

            set_state(DRYDOCK_MIDI_VERIFY, product);
            if (midi_send_sysex(fw_up_upgrade, sizeof(fw_up_upgrade)) != ESP_OK) {
                free(image);
                set_state(DRYDOCK_MIDI_ERROR, "UPGRADE send failed");
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            if (!serve(image, image_len, FW_UP_FINISH_VERIFY, 8000, 0)) {
                free(image);
                set_state(DRYDOCK_MIDI_ERROR, "step 1 stopped (loader not installed)");
                continue;
            }

            /* the FM-1 reboots into the loader: wait for the ota-FM-1_9XX re-attach */
            set_state(DRYDOCK_MIDI_LOADER, NULL);
            {
                int found_ota = 0;
                for (int w = 0; w < 60 && !s_abort; w++) { /* 30 s */
                    vTaskDelay(pdMS_TO_TICKS(500));
                    if (!s_attached) {
                        continue;
                    }
                    if (handshake(id, sizeof(id)) == 0) {
                        snprintf(s_identity, sizeof(s_identity), "%s", id);
                        if (strncmp(id, "ota-", 4) == 0) {
                            found_ota = 1;
                            break;
                        }
                    }
                }
                if (!found_ota) {
                    free(image);
                    set_state(DRYDOCK_MIDI_ERROR, "loader did not appear");
                    continue;
                }
            }

            set_state(DRYDOCK_MIDI_WRITE, NULL);
            if (midi_send_sysex(fw_up_upgrade, sizeof(fw_up_upgrade)) != ESP_OK) {
                free(image);
                set_state(DRYDOCK_MIDI_ERROR, "UPGRADE send (loader) failed");
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            if (!serve(image, image_len, FW_UP_FINISH_WRITE, 180000, 1)) {
                free(image);
                set_state(DRYDOCK_MIDI_ERROR, "write stopped (replug and retry)");
                continue;
            }
            free(image);

            /* the FM-1 reboots into the new firmware */
            set_state(DRYDOCK_MIDI_REBOOT, NULL);
            {
                int back = 0;
                for (int w = 0; w < 80 && !s_abort; w++) { /* 40 s */
                    vTaskDelay(pdMS_TO_TICKS(500));
                    if (!s_attached) {
                        continue;
                    }
                    if (handshake(id, sizeof(id)) == 0) {
                        snprintf(s_identity, sizeof(s_identity), "%s", id);
                        if (strcmp(id, product) == 0) {
                            back = 1;
                            break;
                        }
                    }
                }
                if (!back) {
                    set_state(DRYDOCK_MIDI_ERROR, "the FM-1 didn't come back (power-cycle)");
                    continue;
                }
            }
            set_state(DRYDOCK_MIDI_DONE, product);
            continue;
        }

        /* idle: keeps the identity up to date for the screen */
        if (s_identity[0] == '\0') {
            char id[32];
            if (handshake(id, sizeof(id)) == 0) {
                snprintf(s_identity, sizeof(s_identity), "%s", id);
                set_state(DRYDOCK_MIDI_CONNECTED, id);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ---- public API ---- */

esp_err_t drydock_midi_init(const drydock_midi_callbacks_t *cb)
{
    if (cb != NULL) {
        s_cb = *cb;
    }
    if (s_xfer_done == NULL) {
        s_xfer_done = xSemaphoreCreateBinary();
    }
    if (s_xfer == NULL) {
        ESP_RETURN_ON_ERROR(usb_host_transfer_alloc(XFER_BUF_SIZE, 0, &s_xfer), TAG,
                            "transfer_alloc");
        s_xfer->callback = xfer_cb;
    }
    static int task_started;
    if (!task_started) {
        task_started = 1;
        xTaskCreate(midi_task, "midi_host", 8192, NULL, 4, NULL);
    }
    return ESP_OK;
}

esp_err_t drydock_midi_flash(const char *path)
{
    if (path == NULL || strlen(path) >= sizeof(s_flash_path)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state == DRYDOCK_MIDI_VERIFY || s_state == DRYDOCK_MIDI_WRITE ||
        s_state == DRYDOCK_MIDI_LOADER) {
        return ESP_ERR_INVALID_STATE; /* a flash is in progress */
    }
    snprintf(s_flash_path, sizeof(s_flash_path), "%s", path);
    s_flash_pending = 1;
    s_error[0] = '\0';
    return ESP_OK;
}

esp_err_t drydock_midi_send_enter_bootloader(void)
{
    if (!s_attached) {
        return ESP_ERR_INVALID_STATE;
    }
    return midi_send_sysex(fw_uboot_key, sizeof(fw_uboot_key));
}

const char *drydock_midi_identity(void)
{
    return s_identity;
}

int drydock_midi_connected(void)
{
    return s_attached;
}

drydock_midi_state_t drydock_midi_state(void)
{
    return s_state;
}

const char *drydock_midi_error(void)
{
    return s_error[0] ? s_error : NULL;
}
