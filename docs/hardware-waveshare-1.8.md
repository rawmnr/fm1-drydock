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
| V2 | CO5300 | CST820 |

La page wiki Waveshare décrit la V1 (SH8601 + FT3168). La bibliothèque Arduino officielle inclut aussi un driver CST816, cohérent avec les révisions récentes. [À vérifier] détection logicielle de la révision (readback du contrôleur d'affichage).

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

SDMMC 1 bit, CS piloté par le TCA9554 (EXIO7), format FAT32. [À vérifier] broches SDMMC exactes, vitesse et compatibilité CS contre le BSP esp-waveshare.

## USB-C : Device vs Host

- Device : énumération TinyUSB mesurée (JTAG/CDC). Le mode PC Sync (MSC) s'appuie dessus.
- Host : le même port doit passer en Host (usb_host_lib) pour piloter le FM-1. [À vérifier Phase 0] alimentation de VBUS en autonomie (sans PC branché) et câble USB-C adapté. C'est le point le plus risqué de la Phase 0 ; à valider avant d'engager la Phase 2.
