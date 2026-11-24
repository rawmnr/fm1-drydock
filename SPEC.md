# FM1-Drydock — Spécification Technique & Architecture

## 1. Objectif
**FM1-Drydock** transforme une carte **Waveshare ESP32-S3-Touch-AMOLED-1.8** en une cartouche autonome et tactile capable de stocker et flasher hors-ligne des firmwares (custom, Stock V15 officiel, et Rescue) sur un synthétiseur **M-VAVE FM-1**, avec une synchronisation PC → ESP32 et un flashage ESP32 → FM-1 **100 % USB-C**.

## 2. Matériel cible
### 2.1 Cartouche : Waveshare ESP32-S3-Touch-AMOLED-1.8
- **MCU** : ESP32-S3R8 (16 Mo Flash, 8 Mo OPI PSRAM).
- **Écran** : AMOLED 1.8" 368×448 QSPI.
  - **Révision V1** : contrôleur `SH8601` + tactile `FT3168`.
  - **Révision V2** : contrôleur `CO5300` + tactile `CST820`.
- **Stockage** : partition **Flash interne `drydock_fs`** 12 Mo (`data/fat`, Wear Levelling). Slot microSD en option future (SDMMC 1-bit, CS via expander I2C `TCA9554` EXIO7).
- **PMU** : `AXP2101`.
- **USB-C** : port unique relié à l'USB-OTG natif de l'ESP32-S3 (`GPIO19` D-, `GPIO20` D+).

### 2.2 Cible : M-VAVE FM-1
- **Flash normal** : via **USB-MIDI SysEx** (fichiers `.fwsc`), identique au mécanisme Web MIDI de `thefirmwarehub.com`.
- **Retour Stock** : firmware officiel **FM-1 V15** (Apache-2.0, indispensable comme base propre avant certaines migrations pour éviter les erreurs de partitionnement).
- **Mode Rescue** : bootloader ROM JieLi (`UBOOT1.00`), exposé en **USB Mass Storage (MSC)** avec commandes **SCSI** propriétaires (référence : `fm1_rescue.py` et `jl-uboot-tool`).

## 3. Architecture Dual-Role USB-C
Les piles USB Device (TinyUSB) et USB Host (`usb_host_lib`) ne cohabitant pas dynamiquement sur ESP32-S3, le changement de rôle s'effectue par **redémarrage rapide (~300 ms)** via un flag en RAM RTC (`RTC_NOINIT_ATTR`) :

1. **Mode `HOST_FLASH` (défaut au boot)** :
   - Monte la partition FAT interne **`drydock_fs`** (12 Mo, Wear Levelling) sur `/drydock`.
   - Parse `manifest.json` et affiche le catalogue sur l'AMOLED.
   - Active `usb_host_lib` pour détecter le FM-1 (USB-MIDI en mode normal, USB-MSC `UBOOT1.00` en mode Rescue).
2. **Mode `DEVICE_PC_SYNC` (activé depuis l'UI tactile)** :
   - Écrit le flag RTC et redémarre.
   - **N'accède pas au système de fichiers en local** ; expose la partition `spiflash` en **USB Mass Storage (TinyUSB MSC)** avec le label `DRYDOCK` (exclusion mutuelle stricte avec le VFS local).
   - Le PC synchronise les firmwares via `drydock-sync`.
   - Un tap sur **Done** (ou débranchement) remet le flag sur `HOST_FLASH` et redémarre.

## 4. Contrat de stockage Flash interne (`/drydock`)

Stockage par défaut : partition `drydock_fs` (`data/fat`, 12 Mo) dans la Flash 16 Mo de l'ESP32-S3, montée par `esp_vfs_fat_spiflash_mount_rw_wl()` (`format_if_mount_failed = true` au premier boot). La microSD reste une option future (même layout).

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

### Structure de `manifest.json`

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

## 5. Spécification du compagnon PC (`companion/`)

- **Stack** : Python 3.12+, `uv`, `typer`, `httpx`, `pydantic`.
- **Configuration** : fichier déclaratif `vessels.toml` listant les firmwares souhaités (`firmware_hub`, `github`, `local`).
- **Commandes CLI** :
  - `drydock probe` : inspecte le catalogue distant (`thefirmwarehub.com`) et les périphériques USB connectés au PC (ESP32 / FM-1).
  - `drydock fetch` : télécharge et met en cache local les dernières versions configurées + Stock V15 + helper Rescue.
  - `drydock sync [--target PATH]` : détecte automatiquement le volume USB `DRYDOCK`, copie uniquement les fichiers modifiés (diff SHA-256), vérifie l'intégrité et écrit `manifest.json` de manière atomique.
  - `drydock verify [--target PATH]` : vérifie tous les SHA-256 présents sur la carte SD par rapport au `manifest.json`.

## 6. Spécification du firmware ESP32-S3 (`firmware/`)

- **Stack** : ESP-IDF ≥ 5.5, LVGL 9, BSP Waveshare.
- **Stockage** : `drydock_storage` monte `/drydock` (FAT `drydock_fs` 12 Mo) via wear levelling ; `drydock_usb_msc_device` l'expose au PC en `DEVICE_PC_SYNC`. En ce mode le VFS local n'est pas monté.
- **Écrans UI (368×448 tactile)** :
  1. **Accueil / Liste des Vessels** : liste scrollable (nom, version, catégorie), indicateur de connexion FM-1, accès rapide **PC Sync** et **Rescue / Stock**.
  2. **Détail d'un Vessel** : description, auteur, version, taille, bouton **Flash** (avec option recommandée *Flash Stock V15 first*).
  3. **Progression de Flash** : vérification SHA-256 avant envoi, barre de progression temps réel, statut de vérification finale.
  4. **Rescue & Stock** : bouton dédié **Back to Stock V15**, bouton **Enter Bootloader**, et mode **Rescue `UBOOT1.00`** (flash de récupération).
  5. **PC Sync** : écran d'état USB Mass Storage actif + bouton **Done (Reboot to Dock)**.

## 7. Feuille de route (Phases)

- **Phase 0 — Discovery matériel (HIL sur PC)** :
  - Identifier la révision matérielle de la Waveshare (V1 vs V2) et valider un boot ESP-IDF + écran AMOLED + lecture microSD.
  - Inspecter le FM-1 connecté au PC (VID/PID USB-MIDI, endpoints, structure des paquets `.fwsc` et catalogue JSON de `thefirmwarehub.com`).
  - Valider le lien physique USB-C ESP32 (Host) ↔ FM-1 (énumération, gestion CC1/CC2 et VBUS).
- **Phase 1 — Companion CLI (`companion/`)** :
  - Implémenter `firmware_hub.py`, `models.py`, `syncer.py` et les tests unitaires.
- **Phase 2 — Core USB Firmware (`firmware/`)** :
  - Implémenter `drydock_usb_msc_device` (PC Sync) + `drydock_storage`.
  - Implémenter `drydock_usb_midi_host` (flash `.fwsc` vers FM-1).
  - Implémenter `drydock_usb_rescue_host` (portage C de `fm1_rescue.py` / `jl-uboot-tool`).
- **Phase 3 — UI LVGL & Intégration** :
  - Interface tactile complète, vérification SHA-256 pré-flash, packaging des releases.
