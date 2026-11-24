/* main.c — exécutable de test hôte : extrait la zone d'app d'un .fwsc.
 * Usage: fwsc_extract <file.fwsc> <out.app>
 * Sortie JSON sur stdout : {product, loader, count, app_off, app_size}
 */
#include <stdio.h>
#include <stdlib.h>

#include "fwsc.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <file.fwsc> <out.app>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror("open");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        return 1;
    }
    uint8_t *raw = malloc((size_t)n);
    if (!raw) {
        return 1;
    }
    if (fread(raw, 1, (size_t)n, f) != (size_t)n) {
        return 1;
    }
    fclose(f);

    fwsc_info_t info;
    if (fwsc_parse(raw, (size_t)n, &info) != 0) {
        fprintf(stderr, "%.*s: parse failed\n", 32, raw);
        return 1;
    }

    FILE *out = fopen(argv[2], "wb");
    if (!out) {
        return 1;
    }
    if (info.app_size && fwrite(info.app, 1, info.app_size, out) != info.app_size) {
        return 1;
    }
    fclose(out);

    printf("{\"product\":\"%s\",\"loader\":true,\"count\":%d,\"app_off\":%u,\"app_size\":%u}\n",
           info.product, info.file_count, info.flash_off + FW_APP_LO, info.app_size);
    fwsc_free(&info);
    free(raw);
    return 0;
}
