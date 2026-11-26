# MORNING_REPORT — FM1-Drydock v0.2 (run of 2026-11-24)

Target : complete v0.2 — internal storage, PC Sync MSC, USB Host MIDI/Rescue,
LVGL 9 UI — compiled and validated as much as possible without physical access.
Rules followed : zero writes on the FM-1, atomic commits per milestone.

## Night commits

| Sha | Content |
|---|---|
| `2172c87` | **Milestone 1** : 12 MB internal FAT partition (`drydock_fs`, Wear Levelling) instead of the microSD, VFS on `/drydock`, root layout |
| `6e412db` | **Milestone 2** : DEVICE_PC_SYNC mode — TinyUSB MSC spiflash (`DRYDOCK` volume), dual-mode boot, `pc_sync`/`host` console, DONE & Reboot button, auto-return ~4 s after unplug/eject |
| `7d688c6` | **Milestone 3** : USB Host engines — `drydock_usb_midi_host` (M-UPGRADE : handshake, cmd 0x30, UPGRADE/UBOOT keys), `drydock_usb_rescue_host` (BOT/SCSI, helper loader, chip key, sectorial flash), `drydock_proto` (fwsc/jielib/upgrade, bit-for-bit host test), `drydock_usb_host` (shared lib) |
| `e88cc6c` | **Milestone 4** : LVGL 9 UI — home (vessels list), detail+flash (SHA-256, progress, Stock V15 first), Rescue & Stock, PC Sync, USB Diag, status bar (battery/USB/space) |

## Milestone status

| Milestone | Status | Detail |
|---|---|---|
| 1 Internal storage | **DONE** | `drydock_fs` 12 MB partition mounted as FAT+WL on `/drydock` (`free 12 431 360 bytes`), manifest parsed, clean first boot without microSD. |
| 2 PC Sync MSC | **DONE (partial HIL)** | HIL #2 : `DRYDOCK` volume (D:\), `drydock probe/sync/verify` **all OK** on real hardware (stock + rescue + 3 vessels + manifest < 2 MB). HOST_FLASH auto-return tested on SCSI ejection. |
| 3 Flash/Rescue USB Host | **DONE (code + host tests)** | Complete M-UPGRADE (identity, verify → loader → write → reboot) and Rescue UBOOT1.00 (read-only check + confirmed flash). Protocol measured in the hub JS (`docs/protocol-sysex.md`). Bit-for-bit C ↔ Python host test (gcc CI). |
| 4 LVGL 9 UI | **DONE (compiles)** | 6 screens + status bar, flash callbacks → UI under display lock. Not run on the board (board stuck in MSC, see blockers). |
| 5 Hardening/CI | **DONE** | pytest **53 passed, 0 skipped** (the 10 C host tests run natively under Windows via **zig cc**, bit-for-bit vs the Python references), strict ruff/mypy, `idf.py size` image **861 135 bytes** (~841 KB), partition 3 M, zero-warning build. |

## Blockers / open points

- **The board stayed in PC_SYNC (MSC) mode** : the full HIL test (return to
  HOST_FLASH, manifest parse, bit-for-bit `verify_sha256_streaming`) and the
  flash of the final firmware **could not run**. Windows (non-admin session)
  refuses to remove the device node (« not removable »), so
  `tud_connected()` stays true on the board side. **Workaround = unplug/replug
  the board (or an admin : `pnputil /remove-device "USB\VID_303A&PID_4002\0001"`)**.
  The in-tree firmware already contains the `is_exposed` fix : as soon as the
  next flash happens, a SCSI ejection is enough to trigger the auto-return (no
  need to unplug anymore).
- ~~No Windows C host compiler~~ → resolved : **zig cc** (pip `ziglang`)
  compiles the C protocol and the **10 bit-for-bit tests pass locally**
  (M-UPGRADE framing, JieLi ciphers, SCSI CDB, chip key, `.fwsc` extractor).
- **VBUS Host — resolved by cross-project evidence (fm1-flip)** : the host board does **not** need to supply VBUS. The FM-1 runs on its own battery over a direct C-to-C cable (user-confirmed on a StickS3, see `docs/hardware-waveshare-1.8.md`). Requirements instead : a **plain passive USB-C data cable** (charge-only and active cables never work), the FM-1 **on with its screen lit**, charged battery (a low battery stops its USB). A **bus presence probe** (`drydock_usb_probe`, ported from fm1-flip) now classifies the port at boot (`device` / `empty` / `unstable` / `fault` = charge-only cable or FM-1 off/brown-out/download-mode, cable short) and the USB Diag screen shows the actionable hint.
- **MIDI flash untested on real hardware** : the protocol is ported bit-for-bit
  from the hub JS and verified by the host test, but the real execution on the
  FM-1 is waiting on the board unblocking.
- **Measured FM-1 interface (PC PnP, read-only)** : composite
  `VID_4D4A&PID_4155`, MI_00 = AudioControl `Class_01&SubClass_01` (without
  endpoint, "MEDIA" under Windows) ; the MIDI streaming interface
  `0x01/0x03` does not appear under Windows but exists in the descriptor.
  The host therefore looks for an interface **with IN+OUT endpoints**
  (`drydock_usb_find_data_iface`, fix `5f96058`).

## Morning test protocol (~5 min) — after unplug/replug

1. **Recover the board** : unplug/replug the board USB-C (the RTC_NOINIT flag
   is volatile : a power cycle returns to `HOST_FLASH`, COM6 comes back).
2. **Flash the final firmware** (the one from `e88cc6c`, UI + engines) :
   ```
   cd firmware
   idf.py -p COM6 flash monitor
   ```
3. **Verify the boot** (30 s of log) : V2 revision, AXP2101, FAT mounted
   (`free ~12 MB`), manifest OK (n vessels), `drydock>` console, no
   crash/watchdog. The AMOLED displays the vessels list + status bar.
4. **Complete PC Sync loop** :
   - **PC SYNC** button on the screen (or `pc_sync` console) → restarts in MSC ;
   - the PC sees the `DRYDOCK` volume : `drydock probe`, `drydock sync`,
     `drydock verify` (already validated, replay to confirm) ;
   - eject the volume : the board must **return by itself to HOST_FLASH** (~4 s)
     thanks to the `is_exposed` fix ; check the « USB host left » log.
5. **USB Host (board on its battery, FM-1 on its own battery)** : unplug the board from
   the PC, connect the **FM-1 to the board USB-C with a plain passive USB-C
   data cable** (charge-only and active cables never work; if the FM-1 is on
   with its screen lit but stays undetected, try another cable first). The FM-1
   must be ON and charged — a low battery stops its USB.
   - At boot the log shows `dry_probe: USB bus probe: ...`; the USB Diag screen repeats
     the classification + hint (`empty` = nothing/cable charge-only/FM-1 off,
     `unstable` = FM-1 left in download mode → switch it off and on,
     `fault` = cable short);
   - **USB Diag** screen : `FM-1 (MIDI)`, identity `FM-1_xxx` read ;
   - flash a vessel from the UI (FLASH button) : progress, final status ;
   - **Rescue** : Enter Bootloader button → the FM-1 switches to `UBOOT1.00`,
     Rescue check reads flash id + chip key + sectors to write, without writing
     anything.
6. **Back to Stock V15** : « Back to Stock V15 » button → check the identity
   `FM-1_015` at reboot.

## Morning additions (2026-11-25, fm1-flip feature parity)

- **USB-C bus presence probe** (`drydock_usb_probe`, commit `4e006fa`) : pre-host
  D+/D- classification `device`/`empty`/`unstable`/`fault` with actionable hints
  (charge-only cable, FM-1 off, download-mode leftover, short) — boot log + USB Diag.
- **FM-1 backups** (see `docs/protocol-rescue.md`) : full app-region image
  (568 KiB) with SHA-256, created asynchronously via **Backups** on the home
  screen, auto-connecting through the MIDI UBOOT key. Restore is **hold 2 s**
  confirmed, refuses a chip-key mismatch and creates a fresh rollback backup
  first. Copyable via PC Sync (`/drydock/backups/`, preserved by the syncer).
- **Safety UX** : FLASH / Back to Stock / Restore need a **2-second hold**
  (fm1-flip physical confirmation), Cancel buttons on the flash and backup
  screens stop at the next step / sector boundary.
- Feature comparison with fm1-flip and the declined items (Wi-Fi catalogue,
  web UI, session logs) are in the run records.

## Technical notes

- **New components** : `drydock_proto` (fwsc/jielib/upgrade, pure C host-tested),
  `drydock_usb_host` (shared usb_host_lib), `drydock_usb_midi_host`,
  `drydock_usb_rescue_host`, `drydock_ui`, `drydock_boot` (extracted boot_mode).
- **`/drydock/` layout** : files at the root (no more `drydock/` subfolder) —
  an older flash with a subfolder would show a duality, a re-synchronization is
  required (already done at HIL #2).
- **Console** : USB-SERIAL-JTAG (COM6), `pc_sync` / `host` commands.
- **`CONFIG_TINYUSB_MSC_BUFSIZE=4096`** : do not lower it (≥ WL_SECTOR_SIZE).