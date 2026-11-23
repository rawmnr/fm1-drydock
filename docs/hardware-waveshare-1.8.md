# Carte Waveshare ESP32-S3-Touch-AMOLED-1.8

Statut : probe PC du 2026-11-23, wiki Waveshare, SPEC. Les points marqués [À vérifier] sont à confirmer sur la carte réelle en Phase 0.

## Identité mesurée sur USB

| Périphérique | Instance | Interfaces vues |
|---|---|---|
| ESP32-S3 (carte) | `USB\VID_303A&PID_1001` | MI_00 : CDC série (COM6) ; MI_02 : USB JTAG/serial debug unit |
| FM-1 | `USB\VID_4D4A&PID_4155` | MI_00 : classe MEDIA (USB-MIDI), voir `protocol-sysex.md` |

Le port USB-C de la carte est l'USB natif du S3 (GPIO19 D-, GPIO20 D+). Pas de pont UART externe : le même connecteur sert au téléchargement (bootloader ROM) et à la console série/JTAG.

## Révisions de l'écran AMOLED 368×448

| Révision | Contrôleur affichage | Tactile |
|---|---|---|
| V1 | SH8601 (QSPI) | FT3168 (I2C) |
| V2 | CO5300 | CST816/CST820 |

Le vendeur a annoncé l'arrêt de la V1 et le basculement vers la V2 le 2026-05-30. Le BSP Waveshare officiel pilote la V2 (CO5300 + CST816). Mesuré dans l'exemple officiel `13_display_colorbar` (IDF) :

- **Détection de révision** : probe I2C de l'adresse du tactile CST816 (`0x15`) sur `SDA=15` / `SCL=14`. Adresse répond → V2 ; sinon V1/original. L'écran est piloté en CO5300 dans les deux cas, avec un décalage X de `0x10` si V2.
- **Afficheur V2 (CO5300, QSPI)** : PCLK=11, D0=4, D1=5, D2=6, D3=7, CS=12, RGB565, QSPI.
- **Composant BSP** : `waveshare/esp32_s3_touch_amoled_1_8` (registre, v2.0.3), pour `idf: ">=5.5,<6.1"`. Fournit display + tactile + luminosité + microSD (`bsp_display_start`, `bsp_display_brightness_set`, `bsp_sdcard_mount`).
- **Révision mesurée sur la carte (smoke 1, 2026-11-23) : V2** — `CO5300 + CST816/CST820`. Probe I2C : `CST816=0x15` répond, `FT3168=0x38` absent, log série « board revision: V2 (CO5300 + CST816/CST820) ».

## Périphériques embarqués

| Bloc | Référence | Bus |
|---|---|---|
| PMU | AXP2101 | I2C |
| IMU 6 axes | QMI8658 | I2C |
| RTC | PCF85063 | I2C |
| Audio | ES8311 + haut-parleur + micro | I2C/I2S |
| GPIO expander | TCA9554 | I2C |
| microSD | slot TF | SDMMC (demo LVGL officielle : SDMMC) |
| Flash | 16 MB NOR externe | SPI |
| PSRAM | 8 MB OPI | interne S3R8 |
| Batterie | MX1.25 2P 3.7 V | AXP2101 |
| Boutons | PWR, BOOT | GPIO |

## Stockage microSD

SDMMC 1 bit, CS piloté par le TCA9554 (EXIO7), format FAT32. Le BSP monte la carte à `BSP_SD_MOUNT_POINT` (`/sdcard`) via `bsp_sdcard_mount()` ; la layout Drydock est `/sdcard/drydock/` (voir `SPEC.md`). [À vérifier] broches SDMMC exactes (CMD/CLK/D0) contre le macro `BSP_SD_*` du BSP (smoke 2).

## USB-C : Device vs Host

- Device : énumération TinyUSB mesurée (JTAG/CDC). Le mode PC Sync (MSC) s'appuie dessus.
- Host : le même port doit passer en Host (usb_host_lib) pour piloter le FM-1. [À vérifier Phase 0] alimentation de VBUS en autonomie (sans PC branché) et câble USB-C adapté. C'est le point le plus risqué de la Phase 0 ; à valider avant d'engager la Phase 2.
