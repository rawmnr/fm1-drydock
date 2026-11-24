#include "boot_mode.h"

#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "boot_mode";

#define BOOT_MAGIC 0xD09C4D01u
#define BOOT_CRC_INIT 0xFFFFu

typedef struct {
    uint32_t magic;
    uint16_t crc; /* crc16 des octets ci-dessous } */
    uint8_t mode;
    uint8_t pad;
} boot_flag_t;

RTC_NOINIT_ATTR static boot_flag_t s_flag;

static uint16_t crc16(const uint8_t *data, size_t n)
{
    uint16_t crc = BOOT_CRC_INIT;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
        }
    }
    return crc;
}

static void flag_write(boot_mode_t mode)
{
    s_flag.magic = BOOT_MAGIC;
    s_flag.mode = (uint8_t)mode;
    s_flag.pad = 0;
    s_flag.crc = crc16((const uint8_t *)&s_flag.mode, 2);
}

static bool flag_valid(void)
{
    if (s_flag.magic != BOOT_MAGIC) {
        return false;
    }
    return s_flag.crc == crc16((const uint8_t *)&s_flag.mode, 2);
}

boot_mode_t boot_mode_current(void)
{
    if (!flag_valid()) {
        return BOOT_MODE_HOST_FLASH;
    }
    const uint8_t m = s_flag.mode;
    return (m < BOOT_MODE_COUNT) ? (boot_mode_t)m : BOOT_MODE_HOST_FLASH;
}

boot_mode_t boot_mode_next(void)
{
    return boot_mode_current();
}

void boot_mode_switch(boot_mode_t mode)
{
    if (mode >= BOOT_MODE_COUNT) {
        ESP_LOGE(TAG, "mode invalide %d", (int)mode);
        return;
    }
    ESP_LOGI(TAG, "bascule vers %s puis redémarrage",
             mode == BOOT_MODE_PC_SYNC ? "PC_SYNC" : "HOST_FLASH");
    flag_write(mode);
    esp_restart();
}