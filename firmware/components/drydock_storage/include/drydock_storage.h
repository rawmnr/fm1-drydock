/* drydock_storage — partition FAT interne (Wear Levelling) + manifest.json + SHA-256.
 *
 * Backend : esp_vfs_fat_spiflash_mount_rw_wl sur la partition `drydock_fs`
 * (subtype=fat, 12 Mo), point de montage /drydock. Le nom `drydock_sd_*`
 * est conservé pour préserver l'API publique historique ; le stockage n'est
 * plus une microSD (option future).
 *
 * Contrat partagé : schema/manifest.schema.json <-> models.py (companion) <-> ici.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "wear_levelling.h"

#define DRY_MOUNT_POINT "/drydock"
#define DRY_STOCK_FILE  "stock/fm1_v15_official.fwsc"
#define DRY_RESCUE_FILE "rescue/jl_flash_helper.bin"
#define DRY_MANIFEST    "/drydock/manifest.json"

#define DRY_MAX_VESSELS 16

typedef struct {
    bool valid;            /* 1 si le manifest parse et schema_version == 1 */
    int vessel_count;
    char stock_file[64];
    char stock_sha256[65];
    char rescue_file[64];
    char rescue_sha256[65];
} drydock_manifest_t;

typedef struct {
    char id[24];
    char name[48];
    char version[16];
    char category[24];
    char file[64];
    char sha256[65];
    uint32_t size_bytes;
} drydock_vessel_t;

/** Monte la partition FAT interne (format si absente au premier boot). */
esp_err_t drydock_sd_mount(void);
esp_err_t drydock_sd_unmount(void);

/** Init Wear Levelling seul, sans VFS (mode DEVICE_PC_SYNC : le bloc est
 *  exposé au PC via TinyUSB MSC, le MCU n'y accède pas). */
esp_err_t drydock_fs_wl_init(wl_handle_t *out_wl);

/** Espace libre du système de fichiers monté, en octets (statvfs). */
esp_err_t drydock_fs_free(uint64_t *out_bytes);

/** SHA-256 streaming du fichier (mbedtls), sortie hex 64 + '\0'. Renvoie 0 si OK. */
int drydock_sha256_file(const char *path, char out_hex[65]);

/** Parse /valide manifest.json (cJSON), aligné sur schema/manifest.schema.json. */
int drydock_manifest_parse(const char *path, drydock_manifest_t *out);

/** Liste les vessels (id/name/version/category/file/sha256/size).
 *  Alloue un tableau heap de *count éléments (0 si aucun); l'appelant le libère via free(). */
int drydock_manifest_vessels(const char *path, drydock_vessel_t **out_list);
