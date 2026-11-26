# Waveshare ESP32-S3-Touch-AMOLED-1.8 board

Status : PC probe of 2026-11-23, Waveshare wiki, SPEC. The points marked [To verify] are to be confirmed on the real board in Phase 0.

## Identity measured over USB

| Device | Instance | Interfaces seen |
|---|---|---|
| ESP32-S3 (board) | `USB\VID_303A&PID_1001` | MI_00 : CDC serial (COM6) ; MI_02 : USB JTAG/serial debug unit |
| FM-1 | `USB\VID_4D4A&PID_4155` | MI_00 : MEDIA class (USB-MIDI), see `protocol-sysex.md` |

The board USB-C port is the native USB of the S3 (GPIO19 D-, GPIO20 D+). No external UART bridge : the same connector serves for downloading (ROM bootloader) and the serial/JTAG console.

## AMOLED 368×448 screen revisions

| Revision | Display controller | Touchscreen |
|---|---|---|
| V1 | SH8601 (QSPI) | FT3168 (I2C) |
| V2 | CO5300 | CST816/CST820 |

The vendor announced the end of the V1 and the switch to the V2 on 2026-05-30. The official Waveshare BSP drives the V2 (CO5300 + CST816). Measured in the official example `13_display_colorbar` (IDF) :

- **Revision detection** : I2C probe of the CST816 touchscreen address (`0x15`) on `SDA=15` / `SCL=14`. Address answering → V2 ; otherwise V1/original. The screen is driven in CO5300 in both cases, with an X offset of `0x10` if V2.
- **V2 display (CO5300, QSPI)** : PCLK=11, D0=4, D1=5, D2=6, D3=7, CS=12, RGB565, QSPI.
- **BSP component** : `waveshare/esp32_s3_touch_amoled_1_8` (registry, v2.0.3), for `idf: ">=5.5,<6.1"`. Provides panel and touch initialization, brightness control, and microSD support.
- **Revision measured on the board (smoke 1, 2026-11-23) : V2** — `CO5300 + CST816/CST820`. I2C probe : `CST816=0x15` answers, `FT3168=0x38` absent, serial log « board revision: V2 (CO5300 + CST816/CST820) ».

DryDock initializes the panel and touch through the BSP, then registers the display with `lvgl_port_add_disp()` for QSPI. The BSP's `bsp_display_start()` uses the parallel RGB path, which can release LVGL's draw buffer before the QSPI transfer completes and produce horizontal bands. The SPI path releases the buffer on DMA completion. Its 40-row RGB565 buffer (29,440 bytes) fits in one ESP32-S3 SPI DMA transfer.

## Onboard peripherals

| Block | Reference | Bus |
|---|---|---|
| PMU | AXP2101 | I2C |
| 6-axis IMU | QMI8658 | I2C |
| RTC | PCF85063 | I2C |
| Audio | ES8311 + speaker + mic | I2C/I2S |
| GPIO expander | TCA9554 | I2C |
| microSD | TF slot | SDMMC (official LVGL demo : SDMMC) |
| Flash | 16 MB external NOR | SPI |
| PSRAM | 8 MB OPI | internal S3R8 |
| Battery | MX1.25 2P 3.7 V | AXP2101 |
| Buttons | PWR, BOOT | GPIO |

## Default storage : Internal Flash (`drydock_fs`)

Since the storage switch, Drydock no longer depends on the microSD. The `drydock_fs` partition (`data/fat`, **12 MB**) of the 16 MB Flash is mounted on `/drydock` via `esp_vfs_fat_spiflash_mount_rw_wl()` (Wear Levelling, `format_if_mount_failed = true` on first boot, LFN enabled). Layout : `/drydock/` (see `SPEC.md`). The layout is identical when the board is exposed to the PC in `DEVICE_PC_SYNC` mode (TinyUSB MSC spiflash, `DRYDOCK` volume).

### microSD (future option)

SDMMC 1 bit, CS driven by the TCA9554 (EXIO7), FAT32 format. The BSP mounts the card at `BSP_SD_MOUNT_POINT` (`/sdcard`) via `bsp_sdcard_mount()`. [To verify] exact SDMMC pins (CMD/CLK/D0) against the `BSP_SD_*` macro of the BSP.

## USB-C : Device vs Host

- Device : measured TinyUSB enumeration (JTAG/CDC). The PC Sync (MSC) mode relies on it.
- Host : the same port must switch to Host (usb_host_lib) to drive the FM-1. The board must run on its own battery in this mode (its single USB-C port faces the FM-1).

### VBUS : the FM-1 powers itself, the board must not source VBUS

Cross-project evidence ([fm1-flip](https://github.com/chrisdiana/fm1-flip), M5Stack StickS3, `docs/HARDWARE-VALIDATION.md` and `docs/STICKS3.md`) resolves the former "riskiest point of Phase 0" :

- The host board **does not need to supply VBUS**. The StickS3 does not source power on its USB-C port at all, and a user-confirmed real connection to an FM-1 (running Patchwerk) works over a direct C-to-C cable.
- The FM-1 runs **on its own battery** in that scenario. A battery too low to run its USB looks exactly like "no device" on the host side — an FM-1 that stays undetected must be charged first.
- Neither board nor FM-1 provides VBUS on a C-to-C cable (both are CC pull-down sinks); both sides self-powered is the supported arrangement.

### Cables : passive USB-C data cable required

Ported from fm1-flip's StickS3 guide (user-confirmed failures on their hardware) :

| Cable | Result | Why |
|---|---|---|
| Plain passive USB-C data cable | **works** | data lines wired through |
| Charge-only cable | never works | no data wires at all |
| Active cable (e.g. Meta Quest Link) | never works | its electronics need power from the USB host, which the board does not provide |
| Some ordinary-looking data cables | can fail silently | first reflex when the FM-1 is on with its screen lit but stays undetected: **try another cable** |

### Left-over download mode

An FM-1 left in download mode by an interrupted session (cable pulled mid-flash) **drives its USB lines** instead of connecting; switching it off and on (normal screen back) clears it. `drydock_usb_probe` classifies this state as `unstable` and the USB Diag screen shows the hint.

### Bus presence probe (`drydock_usb_probe`)

Before `drydock_usb_host_start()` takes the PHY, `drydock_usb_bus_probe()` samples D+/D- (GPIO19/20) for ~100 ms with only weak internal pull-downs (~45 kOhm) and classifies the bus : `device` (steady 1.5 kOhm pull-up), `empty` (no device / FM-1 off / battery too low / charge-only cable), `unstable` (FM-1 in download mode or browning out), `fault` (both lines high : cable short). The classification and thresholds are ported from fm1-flip's `src/transport/usb_bus.cpp`; the result is logged at boot and shown on the USB Diag screen with an actionable hint.