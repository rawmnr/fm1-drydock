#include "drydock_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"

static const char *TAG = "dry_storage";

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
    /* BSP : SDMMC 1 bit, CS via TCA9554 EXIO7 (monté par le BSP). */
    return bsp_sdcard_mount();
}

esp_err_t drydock_sd_unmount(void)
{
    return bsp_sdcard_unmount();
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

int drydock_manifest_parse(const char *path, drydock_manifest_t *out)
{
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 256 * 1024) {
        fclose(f);
        return -1;
    }

    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }
    const size_t rd = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
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