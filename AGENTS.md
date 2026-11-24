# AGENTS.md — FM1-Drydock

## Hardware-in-the-Loop (HIL) Environment
The two devices are connected over USB to the development workstation:
1. **Waveshare ESP32-S3-Touch-AMOLED-1.8** (serial/JTAG USB-C port).
2. **M-VAVE FM-1** (USB-MIDI device in normal mode, or USB-MSC `UBOOT1.00` in bootloader mode).

## Hardware safety rules (STRICT)
- **Never write to the M-VAVE FM-1 flash** without explicit confirmation from the user.
- **Read-only** operations on the FM-1 (USB descriptor inspection, MIDI enumeration, `fm1_rescue.py --check`, backup dump) are allowed.
- Before any flash on the ESP32-S3, verify the target COM/tty port so as not to interfere with the FM-1.

## Technical conventions
- **Python (`companion/`)** : exclusively use `uv` (`uv run`, `uv add`, `uv run pytest`). Strict validation via Pydantic v2.
- **Firmware (`firmware/`)** : ESP-IDF ≥ 5.5. Do not use the Arduino framework.
- **Shared contract** : any change to the structure of `manifest.json` must be mirrored simultaneously in `schema/manifest.schema.json`, `companion/src/drydock_sync/models.py` and `firmware/components/drydock_storage/`.

## Recommended execution order to get started
1. **Scaffold** : create the complete directory tree described in `SPEC.md` and `schema/manifest.schema.json`.
2. **Hardware probe** :
   - List the USB devices connected to the PC to identify the ESP32-S3 port and the VID/PID + USB interfaces of the FM-1.
   - Document the results in `docs/protocol-sysex.md` and `docs/hardware-waveshare-1.8.md`.
3. **Remote probe** : inspect the API / JSON catalog of `https://thefirmwarehub.com/` and the script `https://thefirmwarehub.com/rescue/fm1_rescue.py` to implement `companion/src/drydock_sync/providers/firmware_hub.py`.
4. **Incremental validation** : write the unit tests on the `companion/` side before tackling the USB Host stack on the `firmware/` side.