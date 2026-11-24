#include "drydock_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "diskio_wl.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "mbedtls/sha256.h"
#include "wear_levelling.h"

static const char *TAG = "dry_storage";

static wl_handle_t s_wl = WL_INVALID_HANDLE; /* handle partagé avec le futur MSC device */

#define DRY_VOLUME_LABEL "DRYDOCK" /* nom de volume attendu par drydock-sync (VolumeName) */

/* Le label FAT vit dans le boot sector : f_mkfs le laisse vide, Windows le
 * lirait comme "FAT32"/vide et drydock-sync ne trouverait pas le volume. */
static void set_volume_label(void)
{
    const unsigned char pdrv = ff_diskio_get_pdrv_wl(s_wl);
    char drv_label[16];
    snprintf(drv_label, sizeof(drv_label), "%u:%s", (unsigned)pdrv, DRY_VOLUME_LABEL);
    const FRESULT fr = f_setlabel((const TCHAR *)drv_label);
    if (fr != FR_OK) {
        ESP_LOGW(TAG, "f_setlabel(%s) a échoué: %d", drv_label, (int)fr);
    }
}

static int hex_of_stream(FILE *f, char out_hex[65])
{
    mbedtls_sha256_context ctx;
    uint8_t digest[32];
    char buf[4096];
    size_t n;
    int i;

    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0) != 0) {
        mbedtls_sha256_free(&ctx);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (mbedtls_sha256_update(&ctx, (const unsigned char *)buf, n) != 0) {
            mbedtls_sha256_free(&ctx);
            return -1;
        }
    }
    if (ferror(f)) { /* lecture interrompue : ne pas rendre un hash de préfixe comme valide */
        mbedtls_sha256_free(&ctx);
        return -1;
    }
    if (mbedtls_sha256_finish(&ctx, digest) != 0) {
        mbedtls_sha256_free(&ctx);
        return -1;
    }
    mbedtls_sha256_free(&ctx);
    for (i = 0; i < 32; i++) {
        sprintf(out_hex + i * 2, "%02x", digest[i]);
    }
    out_hex[64] = '\0';
    return 0;
}

int drydock_sha256_file(const char *path, char out_hex[65])
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    const int ret = hex_of_stream(f, out_hex);
    fclose(f);
    return ret;
}

esp_err_t drydock_sd_mount(void)
{
    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true, /* premier boot : partition brute -> FAT */
        .max_files = 12,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    const esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(DRY_MOUNT_POINT, "drydock_fs", &cfg, &s_wl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spiflash FAT mount (%s): %s", DRY_MOUNT_POINT, esp_err_to_name(ret));
        return ret;
    }
    set_volume_label();
    uint64_t free_b = 0;
    if (drydock_fs_free(&free_b) == ESP_OK) {
        ESP_LOGI(TAG, "mounted %s, free %llu bytes (%llu MB)",
                 DRY_MOUNT_POINT, (unsigned long long)free_b, (unsigned long long)(free_b >> 20));
    }
    return ESP_OK;
}

esp_err_t drydock_sd_unmount(void)
{
    const esp_err_t ret = esp_vfs_fat_spiflash_unmount_rw_wl(DRY_MOUNT_POINT, s_wl);
    s_wl = WL_INVALID_HANDLE;
    return ret;
}

esp_err_t drydock_fs_free(uint64_t *out_bytes)
{
    uint64_t total = 0;
    const esp_err_t ret = esp_vfs_fat_info(DRY_MOUNT_POINT, &total, out_bytes);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_vfs_fat_info(%s): %s", DRY_MOUNT_POINT, esp_err_to_name(ret));
    }
    return ret;
}

static bool sha_str_ok(const char *s)
{
    if (s == NULL) {
        return false;
    }
    size_t n = strlen(s);
    if (n != 64) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

static void copy_str(char *dst, size_t dst_sz, const char *src)
{
    if (src) {
        snprintf(dst, dst_sz, "%s", src);
    }
}

static cJSON *read_json(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 256 * 1024) {
        fclose(f);
        return NULL;
    }

    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    const size_t rd = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    return root;
}

int drydock_manifest_parse(const char *path, drydock_manifest_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = read_json(path);
    if (root == NULL) {
        return -1;
    }

    const cJSON *ver = cJSON_GetObjectItem(root, "schema_version");
    const cJSON *stock = cJSON_GetObjectItem(root, "stock_v15");
    const cJSON *rescue = cJSON_GetObjectItem(root, "rescue_helper");
    const cJSON *vessels = cJSON_GetObjectItem(root, "vessels");

    if (!cJSON_IsNumber(ver) || ver->valueint != 1 || !cJSON_IsObject(stock) ||
        !cJSON_IsObject(rescue) || !cJSON_IsArray(vessels)) {
        cJSON_Delete(root);
        return -1;
    }

    copy_str(out->stock_file, sizeof(out->stock_file),
             cJSON_GetStringValue(cJSON_GetObjectItem(stock, "file")));
    copy_str(out->stock_sha256, sizeof(out->stock_sha256),
             cJSON_GetStringValue(cJSON_GetObjectItem(stock, "sha256")));
    copy_str(out->rescue_file, sizeof(out->rescue_file),
             cJSON_GetStringValue(cJSON_GetObjectItem(rescue, "file")));
    copy_str(out->rescue_sha256, sizeof(out->rescue_sha256),
             cJSON_GetStringValue(cJSON_GetObjectItem(rescue, "sha256")));

    out->vessel_count = cJSON_GetArraySize(vessels);
    const cJSON *generated_at = cJSON_GetObjectItem(root, "generated_at");
    const bool has_stamp = cJSON_IsString(generated_at) && cJSON_GetStringValue(generated_at)[0] != '\0';

    /* chaque vessel doit avoir file + sha256 hex valides */
    bool vessels_ok = true;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, vessels) {
        if (!cJSON_IsObject(item)) {
            vessels_ok = false;
            break;
        }
        const char *vf = cJSON_GetStringValue(cJSON_GetObjectItem(item, "file"));
        const char *vs = cJSON_GetStringValue(cJSON_GetObjectItem(item, "sha256"));
        if (vf == NULL || !sha_str_ok(vs)) {
            vessels_ok = false;
            break;
        }
    }

    out->valid = (has_stamp && strlen(out->stock_file) > 0 && sha_str_ok(out->stock_sha256) &&
                  strlen(out->rescue_file) > 0 && sha_str_ok(out->rescue_sha256) &&
                  out->vessel_count > 0 && vessels_ok);

    ESP_LOGI(TAG, "manifest: %s, %d vessels, stock=%s", out->valid ? "OK" : "invalid",
             out->vessel_count, out->stock_file);
    cJSON_Delete(root);
    return out->valid ? 0 : -1;
}

int drydock_manifest_vessels(const char *path, drydock_vessel_t **out_list)
{
    *out_list = NULL;
    cJSON *root = read_json(path);
    if (root == NULL) {
        return 0;
    }
    const cJSON *vessels = cJSON_GetObjectItem(root, "vessels");
    if (!cJSON_IsArray(vessels)) {
        cJSON_Delete(root);
        return 0;
    }
    int n = cJSON_GetArraySize(vessels);
    if (n > DRY_MAX_VESSELS) {
        n = DRY_MAX_VESSELS;
    }
    if (n <= 0) {
        cJSON_Delete(root);
        return 0;
    }
    drydock_vessel_t *list = calloc((size_t)n, sizeof(*list));
    if (list == NULL) {
        cJSON_Delete(root);
        return 0;
    }
    int i = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, vessels) {
        if (i >= n || !cJSON_IsObject(item)) {
            continue;
        }
        drydock_vessel_t *v = &list[i];
        copy_str(v->id, sizeof(v->id), cJSON_GetStringValue(cJSON_GetObjectItem(item, "id")));
        copy_str(v->name, sizeof(v->name), cJSON_GetStringValue(cJSON_GetObjectItem(item, "name")));
        copy_str(v->version, sizeof(v->version), cJSON_GetStringValue(cJSON_GetObjectItem(item, "version")));
        copy_str(v->category, sizeof(v->category), cJSON_GetStringValue(cJSON_GetObjectItem(item, "category")));
        copy_str(v->file, sizeof(v->file), cJSON_GetStringValue(cJSON_GetObjectItem(item, "file")));
        copy_str(v->sha256, sizeof(v->sha256), cJSON_GetStringValue(cJSON_GetObjectItem(item, "sha256")));
        const cJSON *sz = cJSON_GetObjectItem(item, "size_bytes");
        v->size_bytes = cJSON_IsNumber(sz) ? (uint32_t)sz->valuedouble : 0;
        if (v->id[0] == '\0') { /* entrée invalide : ne pas la compter */
            continue;
        }
        i++;
    }
    cJSON_Delete(root);
    *out_list = list;
    return i;
}
