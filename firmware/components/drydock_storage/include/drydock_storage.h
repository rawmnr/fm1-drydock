/* drydock_storage — analogique SD + manifest.json + SHA-256 streaming.
 * Contrat partagé : schema/manifest.schema.json <-> models.py (companion) <-> ici.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define DRY_STOCK_FILE  "stock/fm1_v15_official.fwsc"
#define DRY_RESCUE_FILE "rescue/jl_flash_helper.bin"
#define DRY_MANIFEST    "/sdcard/drydock/manifest.json"

typedef struct {
    bool valid;            /* 1 si le manifest parse et schema_version == 1 */
    int vessel_count;
    char stock_file[64];
    char stock_sha256[65];
    char rescue_file[64];
    char rescue_sha256[65];
} drydock_manifest_t;

/** Monte la microSD (BSP). Ne fait pas échouer l'app si la carte est absente. */
esp_err_t drydock_sd_mount(void);
esp_err_t drydock_sd_unmount(void);

/** SHA-256 streaming du fichier (mbedtls), sortie hex 64 + '\0'. Renvoie 0 si OK. */
int drydock_sha256_file(const char *path, char out_hex[65]);

/** Parse /valide manifest.json (cJSON), aligné sur schema/manifest.schema.json. */
int drydock_manifest_parse(const char *path, drydock_manifest_t *out);