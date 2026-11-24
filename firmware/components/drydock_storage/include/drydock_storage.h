/* drydock_storage — internal FAT partition (Wear Levelling) + manifest.json + SHA-256.
 *
 * Backend: esp_vfs_fat_spiflash_mount_rw_wl on the `drydock_fs` partition
 * (subtype=fat, 12 MB), mount point /drydock. The `drydock_sd_*` name
 * is kept to preserve the historical public API; the storage is no longer
 * a microSD (future option).
 *
 * Shared contract: schema/manifest.schema.json <-> models.py (companion) <-> here.
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
    bool valid;            /* 1 if the manifest parses and schema_version == 1 */
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

/** Mounts the internal FAT partition (formats it if absent at first boot). */
esp_err_t drydock_sd_mount(void);
esp_err_t drydock_sd_unmount(void);

/** Init Wear Levelling only, without VFS (DEVICE_PC_SYNC mode: the block is
 *  exposed to the PC via TinyUSB MSC, the MCU doesn't access it). */
esp_err_t drydock_fs_wl_init(wl_handle_t *out_wl);

/** Free space of the mounted filesystem, in bytes (statvfs). */
esp_err_t drydock_fs_free(uint64_t *out_bytes);

/** Streaming SHA-256 of the file (mbedtls), hex output 64 + '\0'. Returns 0 if OK. */
int drydock_sha256_file(const char *path, char out_hex[65]);

/** Parses/validates manifest.json (cJSON), aligned with schema/manifest.schema.json. */
int drydock_manifest_parse(const char *path, drydock_manifest_t *out);

/** Lists the vessels (id/name/version/category/file/sha256/size).
 *  Allocates a heap array of *count elements (0 if none); the caller frees it via free(). */
int drydock_manifest_vessels(const char *path, drydock_vessel_t **out_list);
