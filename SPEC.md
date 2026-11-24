# FM1-Drydock — Technical Specification & Architecture

## 1. Objective
**FM1-Drydock** turns a **Waveshare ESP32-S3-Touch-AMOLED-1.8** board into a standalone touchscreen cartridge able to store and flash firmware offline (custom, official Stock V15, and Rescue) on an **M-VAVE FM-1** synthesizer, with PC → ESP32 synchronization and ESP32 → FM-1 flashing **100% USB-C**.

## 2. Target hardware
### 2.1 Cartridge : Waveshare ESP32-S3-Touch-AMOLED-1.8
- **MCU** : ESP32-S3R8 (16 MB Flash, 8 MB OPI PSRAM).
- **Display** : 1.8" AMOLED 368×448 QSPI.
  - **Revision V1** : `SH8601` controller + `FT3168` touchscreen.
  - **Revision V2** : `CO5300` controller + `CST820` touchscreen.
- **Storage** : **Internal Flash `drydock_fs`** 12 MB partition (`data/fat`, Wear Levelling). Optional microSD slot in the future (SDMMC 1-bit, CS through I2C expander `TCA9554` EXIO7).
- **PMU** : `AXP2101`.
- **USB-C** : single port connected to the native USB-OTG of the ESP32-S3 (`GPIO19` D-, `GPIO20` D+).

### 2.2 Target : M-VAVE FM-1
- **Normal flash** : via **USB-MIDI SysEx** (`.fwsc` files), identical to the Web MIDI mechanism of `thefirmwarehub.com`.
- **Back to Stock** : official **FM-1 V15** firmware (Apache-2.0, essential as a clean base before certain migrations to avoid partitioning errors).
- **Rescue mode** : JieLi ROM bootloader (`UBOOT1.00`), exposed as **USB Mass Storage (MSC)** with proprietary **SCSI** commands (reference : `fm1_rescue.py` and `jl-uboot-tool`).

## 3. Dual-Role USB-C Architecture
Since the USB Device (TinyUSB) and USB Host (`usb_host_lib`) stacks cannot coexist dynamically on the ESP32-S3, the role switch happens through a **quick restart (~300 ms)** via an RTC RAM flag (`RTC_NOINIT_ATTR`) :

1. **`HOST_FLASH` mode (default at boot)** :
   - Mounts the internal **`drydock_fs`** FAT partition (12 MB, Wear Levelling) on `/drydock`.
   - Parses `manifest.json` and displays the catalog on the AMOLED.
   - Enables `usb_host_lib` to detect the FM-1 (USB-MIDI in normal mode, USB-MSC `UBOOT1.00` in Rescue mode).
2. **`DEVICE_PC_SYNC` mode (enabled from the touch UI)** :
   - Writes the RTC flag and restarts.
   - **Does not access the file system locally** ; exposes the `spiflash` partition as **USB Mass Storage (TinyUSB MSC)** with the `DRYDOCK` label (strict mutual exclusion with the local VFS).
   - The PC syncs the firmwares via `drydock-sync`.
   - A tap on **Done** (or unplugging) resets the flag to `HOST_FLASH` and restarts.

## 4. Internal Flash storage contract (`/drydock`)

Default storage : the `drydock_fs` partition (`data/fat`, 12 MB) in the 16 MB Flash of the ESP32-S3, mounted by `esp_vfs_fat_spiflash_mount_rw_wl()` (`format_if_mount_failed = true` on first boot). The microSD remains a future option (same layout).

```text
/drydock/
├── manifest.json
├── stock/
│   └── fm1_v15_official.fwsc
├── rescue/
│   └── jl_flash_helper.bin
├── vessels/
│   ├── sloop.fwsc
│   ├── hortator.fwsc
│   └── skiff.fwsc
└── backups/
```

### `manifest.json` structure

```json
{
  "schema_version": 1,
  "generated_at": "2026-11-23T20:00:00Z",
  "stock_v15": {
    "id": "stock-v15",
    "name": "M-VAVE Official V15",
    "version": "15",
    "file": "stock/fm1_v15_official.fwsc",
    "size_bytes": 0,
    "sha256": "..."
  },
  "rescue_helper": {
    "file": "rescue/jl_flash_helper.bin",
    "sha256": "..."
  },
  "vessels": [
    {
      "id": "sloop",
      "name": "SLOOP",
      "author": "...",
      "version": "2.6",
      "category": "groovebox",
      "description": "Live groovebox for FM-1",
      "source": "firmware_hub",
      "file": "vessels/sloop.fwsc",
      "size_bytes": 0,
      "sha256": "..."
    }
  ]
}
```

## 5. PC companion specification (`companion/`)

- **Stack** : Python 3.12+, `uv`, `typer`, `httpx`, `pydantic`.
- **Configuration** : declarative `vessels.toml` file listing the desired firmwares (`firmware_hub`, `github`, `local`).
- **CLI commands** :
  - `drydock probe` : inspects the remote catalog (`thefirmwarehub.com`) and the USB devices connected to the PC (ESP32 / FM-1).
  - `drydock fetch` : downloads and caches locally the latest configured versions + Stock V15 + Rescue helper.
  - `drydock sync [--target PATH]` : automatically detects the `DRYDOCK` USB volume, copies only the modified files (SHA-256 diff), verifies integrity and writes `manifest.json` atomically.
  - `drydock verify [--target PATH]` : verifies all SHA-256 values present on the SD card against the `manifest.json`.

## 6. ESP32-S3 firmware specification (`firmware/`)

- **Stack** : ESP-IDF ≥ 5.5, LVGL 9, Waveshare BSP.
- **Storage** : `drydock_storage` mounts `/drydock` (FAT `drydock_fs` 12 MB) via wear levelling ; `drydock_usb_msc_device` exposes it to the PC in `DEVICE_PC_SYNC`. In that mode the local VFS is not mounted.
- **UI screens (368×448 touch)** :
  1. **Home / Vessels List** : scrollable list (name, version, category), FM-1 connection indicator, quick access to **PC Sync** and **Rescue / Stock**.
  2. **Vessel Detail** : description, author, version, size, **Flash** button (with the recommended *Flash Stock V15 first* option).
  3. **Flash Progress** : SHA-256 verification before sending, real-time progress bar, final verification status.
  4. **Rescue & Stock** : dedicated **Back to Stock V15** button, **Enter Bootloader** button, and **Rescue `UBOOT1.00`** mode (recovery flash).
  5. **PC Sync** : active USB Mass Storage status screen + **Done (Reboot to Dock)** button.

## 7. Roadmap (Phases)

- **Phase 0 — Hardware discovery (HIL on PC)** :
  - Identify the Waveshare hardware revision (V1 vs V2) and validate an ESP-IDF boot + AMOLED screen + microSD read.
  - Inspect the FM-1 connected to the PC (VID/PID USB-MIDI, endpoints, structure of the `.fwsc` packets and JSON catalog of `thefirmwarehub.com`).
  - Validate the physical USB-C link ESP32 (Host) ↔ FM-1 (enumeration, CC1/CC2 management and VBUS).
- **Phase 1 — Companion CLI (`companion/`)** :
  - Implement `firmware_hub.py`, `models.py`, `syncer.py` and the unit tests.
- **Phase 2 — Core USB Firmware (`firmware/`)** :
  - Implement `drydock_usb_msc_device` (PC Sync) + `drydock_storage`.
  - Implement `drydock_usb_midi_host` (flash `.fwsc` to FM-1) — full M-UPGRADE engine (handshake, serve 0x30, UPGRADE/UBOOT keys).
  - Implement `drydock_usb_rescue_host` (C port of `fm1_rescue.py` / `jl-uboot-tool`) — BOT/SCSI, helper loader, chip key, reread sectorial flash.
- **Phase 3 — LVGL UI & Integration** :
  - Complete touch interface (home, detail+flash, Rescue & Stock, PC Sync, USB Diag, status bar), pre-flash SHA-256 verification, release packaging.