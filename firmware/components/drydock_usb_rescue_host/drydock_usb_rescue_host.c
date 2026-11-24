/* Moteur USB Host rescue : Bulk-Only Transport + protocole bootloader v2
 * (fm1_rescue.py). Portage C, DRY zone only [APP_LO, APP_HI).
 */
#include "drydock_usb_rescue_host.h"

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
#include "jielib.h"

#define JL_LOADER_PATH "/drydock/rescue/jl_flash_helper.bin"

#define MSC_CLASS 0x08
#define MSC_SUBCLASS 0x06
#define MSC_PROTO_BOT 0x50

#define BOT_CBW_SIG 0x43425355u /* 'USBC' */
#define BOT_CSW_SIG 0x53425355u /* 'USBS' */

#define JL_ADDR 0x01C02000u
#define JL_ARG 0x0001u
#define FLASH_IDS 2u
#define APP_LO 0x4000u
#define APP_HI 0x93000u
#define SEC 0x1000u /* secteur 4 Ko */
#define CH 256u     /* tranche d'écriture */
#define HEAD_LEN 0x400u

static const uint8_t s_flash_ids[FLASH_IDS][2] = { { 0x85, 0x60 }, { 0xB3, 0x60 } };

static const char *TAG = "dry_rescue";

static void set_error(const char *msg);

typedef struct {
    usb_device_handle_t dev;
    uint8_t ep_out;
    uint8_t ep_in;
} rescue_dev_t;

static rescue_dev_t s_dev;
static volatile int s_attached;
static char s_error[96];
static drydock_rescue_callbacks_t s_cb;
static SemaphoreHandle_t s_xfer_done;
static usb_transfer_t *s_xfer;
static volatile int s_xfer_status;

/* ---- transport BOT ---- */

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
    return s_xfer_status == USB_TRANSFER_STATUS_COMPLETED ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t bot_submit(const uint8_t *cbw, size_t cbw_len, uint8_t *data, size_t data_len,
                            int dir_in, uint8_t *csw_out, size_t *csw_len, int timeout_ms)
{
    if (!s_attached || s_xfer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* CBW (31 octets) sur OUT */
    s_xfer->device_handle = s_dev.dev;
    s_xfer->bEndpointAddress = s_dev.ep_out;
    s_xfer->num_bytes = (int)cbw_len;
    s_xfer->timeout_ms = 1000;
    memcpy(s_xfer->data_buffer, cbw, cbw_len);
    ESP_RETURN_ON_ERROR(usb_host_transfer_submit(s_xfer), TAG, "CBW submit");
    ESP_RETURN_ON_ERROR(xfer_wait(2000), TAG, "CBW xfer");

    /* données (si phase data) */
    if (data != NULL && data_len > 0) {
        s_xfer->device_handle = s_dev.dev;
        s_xfer->bEndpointAddress = dir_in ? s_dev.ep_in : s_dev.ep_out;
        s_xfer->num_bytes = (int)data_len;
        s_xfer->timeout_ms = (uint32_t)timeout_ms;
        if (!dir_in) {
            memcpy(s_xfer->data_buffer, data, data_len);
        }
        ESP_RETURN_ON_ERROR(usb_host_transfer_submit(s_xfer), TAG, "data submit");
        ESP_RETURN_ON_ERROR(xfer_wait(timeout_ms + 2000), TAG, "data xfer");
        if (dir_in) {
            memcpy(data, s_xfer->data_buffer, (size_t)s_xfer->actual_num_bytes);
        }
    }

    /* CSW (13 octets) sur IN */
    s_xfer->device_handle = s_dev.dev;
    s_xfer->bEndpointAddress = s_dev.ep_in;
    s_xfer->num_bytes = 13;
    s_xfer->timeout_ms = 2000;
    ESP_RETURN_ON_ERROR(usb_host_transfer_submit(s_xfer), TAG, "CSW submit");
    ESP_RETURN_ON_ERROR(xfer_wait(3000), TAG, "CSW xfer");
    if (csw_out != NULL && s_xfer->actual_num_bytes >= 13) {
        memcpy(csw_out, s_xfer->data_buffer, 13);
        if (csw_len) {
            *csw_len = 13;
        }
    }
    return ESP_OK;
}

/* Exécute un CDB SCSI : construit le CBW, gère la phase data, lit le CSW.
 * Renvoie 0 si CSW status OK. */
static int scsi_run(const uint8_t cdb[16], uint8_t *data, size_t data_len, int dir_in,
                    int timeout_ms)
{
    uint8_t cbw[31];
    memset(cbw, 0, sizeof(cbw));
    cbw[0] = 0x55; /* dCBWSignature 'USBC' LE */
    cbw[1] = 0x53;
    cbw[2] = 0x42;
    cbw[3] = 0x43;
    cbw[4] = 0x01; /* dCBWTag */
    cbw[8] = (uint8_t)(data_len & 0xFF);         /* dCBWDataTransferLength */
    cbw[9] = (uint8_t)((data_len >> 8) & 0xFF);
    cbw[10] = (uint8_t)((data_len >> 16) & 0xFF);
    cbw[11] = (uint8_t)((data_len >> 24) & 0xFF);
    cbw[12] = dir_in ? 0x80 : 0x00;              /* bmCBWFlags */
    cbw[15] = 16;                                /* bCBWCBLength */
    memcpy(cbw + 16, cdb, 16);

    uint8_t csw[13] = { 0 };
    size_t csw_len = 0;
    const esp_err_t ret = bot_submit(cbw, sizeof(cbw), data, data_len, dir_in, csw, &csw_len,
                                     10000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bot_submit: %s", esp_err_to_name(ret));
        return -1;
    }
    if (csw_len != 13 || csw[0] != 0x55 || csw[1] != 0x53 || csw[2] != 0x42 || csw[3] != 0x53) {
        ESP_LOGE(TAG, "CSW invalide");
        return -1;
    }
    if (csw[12] != 0) {
        ESP_LOGE(TAG, "SCSI status 0x%02X", csw[12]);
        return -1;
    }
    return 0;
}

/* ---- protocole bootloader v2 (fm1_rescue.py Boot) ---- */

static int bl_mem_write(uint32_t addr, const uint8_t *data, size_t len)
{
    uint8_t args[9];
    args[0] = (uint8_t)(addr >> 24);
    args[1] = (uint8_t)(addr >> 16);
    args[2] = (uint8_t)(addr >> 8);
    args[3] = (uint8_t)addr;
    args[4] = (uint8_t)(len >> 8);
    args[5] = (uint8_t)len;
    args[6] = 0x00;
    args[7] = (uint8_t)(fw_crc16(data, len, 0) & 0xFF);
    args[8] = (uint8_t)((fw_crc16(data, len, 0) >> 8) & 0xFF);
    uint8_t cdb[16];
    fw_cdb(0xFB, 0x06, args, sizeof(args), cdb);
    return scsi_run(cdb, (uint8_t *)data, len, 0, 10000);
}

static int bl_mem_read(uint32_t addr, uint8_t *out, size_t n)
{
    uint8_t args[6];
    args[0] = (uint8_t)(addr >> 24);
    args[1] = (uint8_t)(addr >> 16);
    args[2] = (uint8_t)(addr >> 8);
    args[3] = (uint8_t)addr;
    args[4] = (uint8_t)(n >> 8);
    args[5] = (uint8_t)n;
    uint8_t cdb[16];
    fw_cdb(0xFD, 0x07, args, sizeof(args), cdb);
    return scsi_run(cdb, out, n, 1, 10000);
}

static int bl_mem_jump(uint32_t addr, uint16_t arg)
{
    uint8_t args[6];
    args[0] = (uint8_t)(addr >> 24);
    args[1] = (uint8_t)(addr >> 16);
    args[2] = (uint8_t)(addr >> 8);
    args[3] = (uint8_t)addr;
    args[4] = (uint8_t)(arg >> 8);
    args[5] = (uint8_t)arg;
    uint8_t cdb[16];
    fw_cdb(0xFB, 0x08, args, sizeof(args), cdb);
    uint8_t resp[16];
    return scsi_run(cdb, resp, 16, 1, 10000);
}

static int bl_read_id(uint8_t out[16])
{
    uint8_t cdb[16];
    fw_cdb(0xFC, 0x0B, NULL, 0, cdb);
    return scsi_run(cdb, out, 16, 1, 10000);
}

static int bl_flash_read(uint32_t addr, uint8_t *out, size_t n)
{
    uint8_t args[6];
    args[0] = (uint8_t)(addr >> 24);
    args[1] = (uint8_t)(addr >> 16);
    args[2] = (uint8_t)(addr >> 8);
    args[3] = (uint8_t)addr;
    args[4] = (uint8_t)(n >> 8);
    args[5] = (uint8_t)n;
    uint8_t cdb[16];
    fw_cdb(0xFD, 0x05, args, sizeof(args), cdb);
    return scsi_run(cdb, out, n, 1, 10000);
}

static int bl_flash_erase(uint32_t addr)
{
    uint8_t args[4];
    args[0] = (uint8_t)(addr >> 24);
    args[1] = (uint8_t)(addr >> 16);
    args[2] = (uint8_t)(addr >> 8);
    args[3] = (uint8_t)addr;
    uint8_t cdb[16];
    fw_cdb(0xFB, 0x01, args, sizeof(args), cdb);
    uint8_t resp[16];
    return scsi_run(cdb, resp, 16, 1, 30000);
}

static int bl_flash_write(uint32_t addr, const uint8_t *data, size_t len)
{
    uint8_t args[9];
    args[0] = (uint8_t)(addr >> 24);
    args[1] = (uint8_t)(addr >> 16);
    args[2] = (uint8_t)(addr >> 8);
    args[3] = (uint8_t)addr;
    args[4] = (uint8_t)(len >> 8);
    args[5] = (uint8_t)len;
    args[6] = 0x00;
    const uint16_t crc = fw_crc16(data, len, 0);
    args[7] = (uint8_t)(crc & 0xFF);
    args[8] = (uint8_t)((crc >> 8) & 0xFF);
    uint8_t cdb[16];
    fw_cdb(0xFB, 0x04, args, sizeof(args), cdb);
    return scsi_run(cdb, (uint8_t *)data, len, 0, 10000);
}

static int bl_run_app(void)
{
    uint8_t args[4] = { 0, 0, 0, 1 };
    uint8_t cdb[16];
    fw_cdb(0xFC, 0x0C, args, sizeof(args), cdb);
    uint8_t resp[16];
    return scsi_run(cdb, resp, 16, 1, 3000);
}

/* Clé chip : scanne la tête flash (JLFS), dérive la clé du blob isd_config.ini. */
static int read_chip_key(uint16_t *out_key)
{
    uint8_t head[HEAD_LEN];
    if (bl_flash_read(0, head, HEAD_LEN) != 0) {
        return -1;
    }
    for (size_t off = 32; off < HEAD_LEN; off += 32) {
        uint8_t e[32];
        memcpy(e, head + off, 32);
        fw_enc(e, 0, 32, 0xFFFF);
        const uint16_t crc = (uint16_t)(e[0] | (e[1] << 8));
        if (fw_crc16(e + 2, 30, 0) != crc) {
            set_error("tête non-JieLi (CRC)");
            return -1;
        }
        if (memcmp(e + 16, "isd_config.ini\0", 15) == 0) {
            const uint32_t blob_off = (uint32_t)e[4] | ((uint32_t)e[5] << 8) |
                                      ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
            uint8_t blob[34];
            if (bl_flash_read(blob_off, blob, 34) != 0) {
                return -1;
            }
            if (fw_chip_key(blob, out_key) != 0) {
                set_error("blob clé chip endommagé");
                return -1;
            }
            return 0;
        }
        if ((uint16_t)(e[14] | (e[15] << 8)) != 0) {
            break;
        }
    }
    set_error("pas de clé chip dans la tête flash");
    return -1;
}

static void set_error(const char *msg)
{
    snprintf(s_error, sizeof(s_error), "%s", msg);
    ESP_LOGE(TAG, "%s", msg);
}

/* ---- driver USB Host ---- */

static int match_mass_storage(const usb_device_desc_t *dev, const usb_config_desc_t *cfg)
{
    (void)dev;
    return drydock_usb_find_interface(cfg, MSC_CLASS, MSC_SUBCLASS, MSC_PROTO_BOT) != NULL;
}

static esp_err_t attach_msc(usb_host_client_handle_t client, usb_device_handle_t dev,
                            const usb_config_desc_t *cfg)
{
    const usb_intf_desc_t *iface = drydock_usb_find_interface(cfg, MSC_CLASS, MSC_SUBCLASS,
                                                              MSC_PROTO_BOT);
    if (iface == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(
        usb_host_interface_claim(client, dev, iface->bInterfaceNumber, iface->bAlternateSetting),
        TAG, "claim iface MSC");
    const usb_ep_desc_t *ep_out = drydock_usb_find_ep(iface, 1);
    const usb_ep_desc_t *ep_in = drydock_usb_find_ep(iface, 0);
    if (ep_out == NULL || ep_in == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_dev.dev = dev;
    s_dev.ep_out = ep_out->bEndpointAddress;
    s_dev.ep_in = ep_in->bEndpointAddress;
    s_attached = 1;
    return ESP_OK;
}

static void detach_msc(void)
{
    s_attached = 0;
    s_dev.dev = NULL;
}

static const drydock_usb_driver_t s_driver = {
    .name = "fm1-uboot",
    .match = match_mass_storage,
    .attach = attach_msc,
    .detach = detach_msc,
};

const drydock_usb_driver_t *drydock_rescue_driver(void)
{
    return &s_driver;
}

/* Charge le helper loader en RAM (0x01C02000) s'il n'y tourne pas déjà.
 * Séquences bit-à-bit de fm1_rescue.py : lecture mémoire = MengLi(chiffré) si
 * le ROM répond, clair si le helper tourne déjà. Le helper est transmis par
 * blocs de 512 octets. */
static int helper_ensure_loaded(void)
{
    FILE *f = fopen(JL_LOADER_PATH, "rb");
    if (f == NULL) {
        set_error("helper loader absent de /drydock/rescue/");
        return -1;
    }
    fseek(f, 0, SEEK_END);
    const long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen <= 0 || flen > 65536) {
        fclose(f);
        set_error("helper loader invalide");
        return -1;
    }
    uint8_t *jl = malloc((size_t)flen);
    if (jl == NULL || fread(jl, 1, (size_t)flen, f) != (size_t)flen) {
        fclose(f);
        free(jl);
        return -1;
    }
    fclose(f);

    /* déjà chargé ? le ROM répondrait chiffré MengLi, le helper en clair */
    uint8_t probe[16];
    if (bl_mem_read(JL_ADDR, probe, 16) == 0) {
        uint8_t enc[16];
        memcpy(enc, jl, 16);
        fw_mengli(enc, 16);
        if (memcmp(probe, enc, 16) == 0) {
            ESP_LOGI(TAG, "helper déjà chargé");
            free(jl);
            return 0;
        }
    }

    for (size_t off = 0; off < (size_t)flen; off += 512) {
        const size_t chunk = (size_t)flen - off < 512 ? (size_t)flen - off : 512;
        if (bl_mem_write(JL_ADDR + (uint32_t)off, jl + off, chunk) != 0) {
            set_error("écriture helper RAM échouée");
            free(jl);
            return -1;
        }
    }
    if (bl_mem_jump(JL_ADDR, JL_ARG) != 0) {
        set_error("jump helper échoué");
        free(jl);
        return -1;
    }
    ESP_LOGI(TAG, "helper loader chargé (%ld octets)", flen);
    free(jl);
    return 0;
}

/* ---- INQUIRY + vérification d'identité ---- */

static int inquiry(char *vendor, size_t v_sz, char *model, size_t m_sz)
{
    uint8_t cdb[16] = { 0x12, 0, 0, 0, 36, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t d[36] = { 0 };
    if (scsi_run(cdb, d, 36, 1, 10000) != 0) {
        return -1;
    }
    snprintf(vendor, v_sz, "%.8s", (char *)d + 8);
    snprintf(model, m_sz, "%.16s", (char *)d + 16);
    for (size_t i = 0; i < v_sz; i++) {
        if (vendor[i] == ' ') {
            vendor[i] = '\0';
        }
    }
    for (size_t i = 0; i < m_sz; i++) {
        if (model[i] == ' ') {
            model[i] = '\0';
        }
    }
    return 0;
}

/* ---- API ---- */

esp_err_t drydock_rescue_init(const drydock_rescue_callbacks_t *cb)
{
    if (cb != NULL) {
        s_cb = *cb;
    }
    if (s_xfer_done == NULL) {
        s_xfer_done = xSemaphoreCreateBinary();
    }
    if (s_xfer == NULL) {
        ESP_RETURN_ON_ERROR(usb_host_transfer_alloc(1024, 0, &s_xfer), TAG, "transfer_alloc");
        s_xfer->callback = xfer_cb;
    }
    return ESP_OK;
}

int drydock_rescue_connected(void)
{
    return s_attached;
}

const char *drydock_rescue_error(void)
{
    return s_error[0] ? s_error : NULL;
}

static esp_err_t load_app_area(const char *fwsc_path, uint8_t **app_out, uint32_t *app_len,
                               char *product, size_t product_sz)
{
    char path[160];
    snprintf(path, sizeof(path), "/drydock/%s", fwsc_path);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        set_error("paquet introuvable sur /drydock");
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    const long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen <= 0) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *raw = heap_caps_malloc((size_t)flen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (raw == NULL || fread(raw, 1, (size_t)flen, f) != (size_t)flen) {
        fclose(f);
        if (raw) {
            free(raw);
        }
        return ESP_ERR_NO_MEM;
    }
    fclose(f);

    fwsc_info_t info;
    if (fwsc_parse(raw, (size_t)flen, &info) != 0) {
        free(raw);
        set_error("paquet invalide (pas Felucca ?)");
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(product, product_sz, "%s", info.product);
    uint8_t *app = heap_caps_malloc(info.app_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (app == NULL) {
        fwsc_free(&info);
        free(raw);
        return ESP_ERR_NO_MEM;
    }
    memcpy(app, info.app, info.app_size);
    *app_out = app;
    *app_len = info.app_size;
    fwsc_free(&info);
    free(raw);
    return ESP_OK;
}

esp_err_t drydock_rescue_check(const char *fwsc_path, drydock_rescue_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_attached) {
        return ESP_ERR_INVALID_STATE;
    }
    if (inquiry(out->vendor, sizeof(out->vendor), out->model, sizeof(out->model)) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (strcmp(out->model, "UBOOT1.00") != 0 || strcmp(out->vendor, "WL82") != 0) {
        set_error("pas un UBOOT1.00 (WL82)");
        return ESP_ERR_NOT_SUPPORTED;
    }
    out->found = 1;

    /* le helper accélère les lectures flash ; ne jamais le réécrire s'il tourne */
    if (helper_ensure_loaded() == 0) {
        out->helper_running = 1;
    }

    uint8_t rid[16];
    if (bl_read_id(rid) == 0) {
        for (size_t i = 0; i < FLASH_IDS; i++) {
            if (rid[1] == s_flash_ids[i][0] && rid[2] == s_flash_ids[i][1]) {
                out->flash_ok = 1;
                break;
            }
        }
        ESP_LOGI(TAG, "flash id: %02X %02X %02X", rid[0], rid[1], rid[2]);
    }

    uint16_t key = 0;
    if (read_chip_key(&key) == 0) {
        out->chip_key = key;
        out->head_crc_ok = 1;
    }

    if (fwsc_path != NULL && fwsc_path[0]) {
        uint8_t *app = NULL;
        uint32_t app_len = 0;
        char product[FW_MAX_PRODUCT + 1];
        if (load_app_area(fwsc_path, &app, &app_len, product, sizeof(product)) == ESP_OK) {
            /* le paquet est-il pour cette clé ? SFC + CRC du premier bloc */
            const size_t blk = app_len > 32 ? 32 : app_len;
            uint8_t hdr[32];
            memcpy(hdr, app, blk);
            fw_sfc(hdr, blk, out->chip_key);
            if (fw_crc16(hdr + 2, blk - 2, 0) == (uint16_t)(hdr[0] | (hdr[1] << 8))) {
                out->sectors_total = (APP_HI - APP_LO) / SEC;
                uint8_t cur[SEC];
                for (uint32_t s = APP_LO; s < APP_HI; s += SEC) {
                    if (bl_flash_read(s, cur, SEC) == 0 &&
                        memcmp(cur, app + (s - APP_LO), SEC) != 0) {
                        out->sectors_diff++;
                    }
                }
            }
            free(app);
        }
    }
    return ESP_OK;
}

esp_err_t drydock_rescue_flash(const char *fwsc_path, int confirm)
{
    if (!s_attached) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!confirm) {
        set_error("confirmation requise");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *app = NULL;
    uint32_t app_len = 0;
    char product[FW_MAX_PRODUCT + 1];
    ESP_RETURN_ON_ERROR(load_app_area(fwsc_path, &app, &app_len, product, sizeof(product)),
                        TAG, "load app area");

    /* helper loader en RAM (jamais réécrit s'il tourne déjà) */
    if (helper_ensure_loaded() != 0) {
        free(app);
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t key = 0;
    if (read_chip_key(&key) != 0) {
        free(app);
        return ESP_ERR_INVALID_STATE;
    }

    /* le paquet doit se décrypter avec la clé du FM-1 */
    uint8_t blk0[32];
    memcpy(blk0, app, 32);
    fw_sfc(blk0, 32, key);
    if (fw_crc16(blk0 + 2, 30, 0) != (uint16_t)(blk0[0] | (blk0[1] << 8))) {
        set_error("paquet pas pour cette clé chip");
        free(app);
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "clé chip 0x%04X, paquet %s validé", key, product);

    /* secteurs à écrire : uniquement ceux qui diffèrent */
    uint8_t cur[SEC];
    uint8_t want[SEC];
    uint32_t n_todo = 0;
    for (uint32_t s = APP_LO; s < APP_HI; s += SEC) {
        if (bl_flash_read(s, cur, SEC) == 0 &&
            memcmp(cur, app + (s - APP_LO), SEC) != 0) {
            n_todo++;
        }
    }
    if (n_todo == 0) {
        ESP_LOGI(TAG, "rien à écrire : le FM-1 a déjà ce firmware");
        free(app);
        bl_run_app();
        return ESP_OK;
    }
    ESP_LOGI(TAG, "%u secteurs à écrire", (unsigned)n_todo);

    uint32_t done = 0;
    for (uint32_t s = APP_LO; s < APP_HI; s += SEC) {
        if (bl_flash_read(s, cur, SEC) == 0 &&
            memcmp(cur, app + (s - APP_LO), SEC) == 0) {
            continue;
        }
        memcpy(want, app + (s - APP_LO), SEC);
        int ok = 0;
        for (int try_ = 0; try_ < 3 && !ok; try_++) {
            if (bl_flash_erase(s) != 0) {
                continue;
            }
            int w_ok = 1;
            for (size_t o = 0; o < SEC; o += CH) {
                if (bl_flash_write(s + (uint32_t)o, want + o, CH) != 0) {
                    w_ok = 0;
                    break;
                }
            }
            if (w_ok && bl_flash_read(s, cur, SEC) == 0 &&
                memcmp(cur, want, SEC) == 0) {
                ok = 1;
            }
        }
        if (!ok) {
            set_error("secteur non relu conforme (reprise possible)");
            free(app);
            return ESP_FAIL;
        }
        done++;
        if (s_cb.on_progress) {
            s_cb.on_progress((int)done, (int)n_todo, "write");
        }
    }

    /* vérification finale complète */
    int bad = 0;
    for (uint32_t s = APP_LO; s < APP_HI && !bad; s += SEC) {
        if (bl_flash_read(s, cur, SEC) == 0 &&
            memcmp(cur, app + (s - APP_LO), SEC) != 0) {
            bad = 1;
        }
    }
    free(app);
    if (bad) {
        set_error("vérification finale échouée");
        return ESP_FAIL;
    }
    bl_run_app();
    ESP_LOGI(TAG, "flash terminé, %s lancé", product);
    return ESP_OK;
}
