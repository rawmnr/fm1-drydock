# JieLi Rescue (UBOOT1.00 / SCSI)

Reference : `fm1_rescue.py` (GPL-3.0) and `jl-uboot-tool` (MIT, kagaimiq). Ported to the ESP32-S3 rescue flash : `drydock_usb_rescue_host`.

## When

An FM-1 that no installer can find anymore, or an interrupted update. The bootloader is in the ROM of the AC791N chip (mask ROM) : no firmware can overwrite it.

## Entering the bootloader

1. Via USB-MIDI : send `F0 22 24 35 7D F7` (« Send to the bootloader » button on the hub page, Chrome/Edge). Requires a firmware still running.
2. At power-on : hold OCT− during power-up (hub FAQ, « USB rescue »).

Result : the FM-1 enumerates as a USB disk. Vendor `WL82`, model `UBOOT1.00`. Nothing is written ; cutting the power restores the original firmware.

## Transport

SCSI pass-through. Linux : `/dev/sg*` via the SG_IO ioctl. Windows and Mac : `MvaveFM1Unbricker` (Quixotic7, MIT) uses the same path. The ESP32-S3 port uses `usb_host_msc` in Host mode.

## Command set

16-byte CDB : `op`, `sub`, arguments, then `0xFF` up to 16. Protocol v2.

| Operation | op | sub | Arguments | Payload |
|---|---|---|---|---|
| Read RAM memory | `0xFD` | `0x07` | addr BE32, n BE16 | output n bytes |
| Write RAM memory | `0xFB` | `0x06` | addr BE32, len BE16, `0x00`, crc16 LE16 | data |
| Jump memory | `0xFB` | `0x08` | addr BE32, arg BE16 | output 16 bytes |
| Read flash id | `0xFC` | `0x0B` | — | 16 bytes, one-byte-shifted answer (`00 85 60 ...`) |
| Read flash | `0xFD` | `0x05` | addr BE32, n BE16 | data |
| Erase sector | `0xFB` | `0x01` | addr BE32 | — (30 s timeout) |
| Write flash | `0xFB` | `0x04` | addr BE32, len BE16, `0x00`, crc16 LE16 | data |
| Launch the app | `0xFC` | `0x0C` | arg BE32 = 1 | output 16 bytes |

## JieLi ciphers

- CRC-16/XMODEM, poly `0x1021` : packet, flash records, bootloader.
- ENC : stream cipher (xor + LFSR feedback, poly `0x1021`).
- SFC : ENC in 32-byte blocks, key xored with `offset >> 2`.
- MengLi : CRC cipher of the ROM for RAM memory accesses (`crc16` chained with the GB2312 magic `孟黎我爱你，玉林`). The ROM answers MengLi-encrypted ; the loaded helper answers in clear.

## Helper loader

- File : `wl82loader.bin`, commit `adb3f18889e88ac512ce0a3c4d8cc3d3cb30696a` of `jl-uboot-tool`.
- SHA-256 : `d41da6126760c9d66660bcc0cac8d27d221806c5e369a8036921efe68dca5376`.
- Loaded in RAM `0x01C02000`, argument `0x0001` (SPI NOR). Transmitted in blocks of 512 MengLi-encrypted bytes.
- Resuming : if `mem_read(JL_ADDR, 16)` returns the bytes in clear, the helper is already running, do not rewrite it.

## Flash

- Expected chip : JEDEC `0x8560` (Puya, older units) or `0xB360` (UC25HQ80, recent units).
- Layout : the header `0x0000..0x3FFF` is never written (SPL, chip key, JLFS entries). App zone `0x4000..0x92FFF`. 4 KB sectors.
- Writing : only the sectors that differ. Per sector : erase, write in 256-byte slices, read-back. Three attempts. Resumable if the session is interrupted. Full final verification, then `run_app`.

## Chip key

Read from the FM-1 header itself, never from the packet :

- JLFS entry `isd_config.ini` (32 ENC-encrypted bytes, CRC16) points to a 34-byte blob (32 + CRC16).
- Derivation : `s = sum(blob[:16]) & 0xFF`, clamped to `0x55` below `0x10` and `0xAA` above `0xE0`. Bit `i` = 1 if `(blob[16+i] ^ blob[15-i]) < s`.

A packet only applies if its app zone decrypts with this key (`sfc` + CRC16 of the first block).

## Invariants for the port

1. Never write the flash header.
2. Verify before writing, in order : flash chip, chip key, packet. Otherwise stop without writing.
3. Each written sector is reread. Failure = stop, resumable at the next run.
4. Keep a read-only mode (`--check` equivalent) : read, compare, write nothing.
5. Require explicit confirmation before writing.