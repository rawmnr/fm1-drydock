# Flashing the FM-1 in normal mode (USB-MIDI SysEx)

Sources : PC probe of 2026-11-23, Web MIDI script of `thefirmwarehub.com`
(measured on 2026-11-24), `fm1_rescue.py`, hub catalog.

## Identity measured over USB

FM-1 in normal mode : `USB\VID_4D4A&PID_4155`, MEDIA class (USB-MIDI device),
MI_00 interface of a composite. The update protocol is **M-UPGRADE** :
it uses the standard MIDI port (SysEx), and therefore the same path as the Web
MIDI of the hub page (Chrome/Edge).

## M-UPGRADE update protocol (measured in the hub JS)

The C port lives in `firmware/components/drydock_proto/upgrade.{c,h}` and the
USB Host engine in `firmware/components/drydock_usb_midi_host/`.

USB-MIDI events are four bytes. In the header byte, the cable number is
the high nibble and the CIN is the low nibble. Drydock uses cable 0.
Per the [USB-MIDI 1.0 specification, Table 4-1](https://www.usb.org/sites/default/files/midi10.pdf),
CIN `0x4` carries three starting or continuing SysEx bytes. CIN `0x5`,
`0x6`, and `0x7` end a SysEx with one, two, and three bytes respectively,
including `F7`. The receiver discards unused bytes in the final event.

### Handshake (identity)

Send `HS_QUERY` :

```
F0 00 32 45 00 00 00 40 7F F7
```

The FM-1 answers with a SysEx whose unpacked (pack7) body is 34 bytes,
`00 59 11` at the head, text at bytes 6..32 (NUL-terminated) : the identity of
the firmware, format `NAME_ddd` (e.g. `FM-1_900`, `SLOOP_26`). The update
loader answers `ota-FM-1_9XX` (`IS_OTA` in the hub).

### Keys

| Key | SysEx | Effect |
|---|---|---|
| `UPGRADE` | `F0 22 24 35 7F F7` | firmware : installs the loader ; loader : starts the write |
| `UBOOT_KEY` | `F0 22 24 35 7D F7` | switches to `UBOOT1.00` rescue mode (see `protocol-rescue.md`) |

Do not confuse the two (0x7F vs 0x7D).

### Read packets (cmd 0x30)

During an update the FM-1 sends read requests on the **logical image** (the
`.fwsc` without the 20 identity marker bytes). Each request is a SysEx whose
unpacked (pack7) body is **15 bytes** :

```
00 59 30 xx xx 00 fl a3 a2 a1 a0 l2 l1 l0 ck
```

- `addr` (bytes 7..10, LE32) and `len` (bytes 11..13, LE24) designate the
  range read in the logical image.
- `ck = ~sum(bytes 6..13) & 0xFF` (8-bit checksum).
- Special addresses : `0xE0000000` = end of step 1 (the firmware has installed
  the loader), `0xF0000000` = end of step 2 (write finished).

### Answer

Unpacked body `14 + len + 1` bytes :

```
00 59 30 (len+8)lo (len+8)hi 00 fl a3 a2 a1 a0 llo lhi 00 data... ck
```

`ck = ~sum(bytes 6..end of data) & 0xFF`. The whole is pack7 and wrapped in
`F0 .. F7`. Max `len` : 512 bytes (`MAXDATA`). At the end addresses, answer
`"success"` (7 bytes + checksum).

### Installation sequence (identical to the hub)

1. Handshake : non-OTA identity ; check that the packet model matches
   (`FM-1_xxx` → FM-1).
2. Send `UPGRADE`, wait ~2 s.
3. Serve the reads (end `0xE0000000`, inactivity 8 s) — the firmware
   verifies the packet and installs the loader.
4. The FM-1 restarts into the loader : wait for the `ota-FM-1_9XX` identity (30 s).
5. Send `UPGRADE`, wait ~2 s, serve the reads (end `0xF0000000`,
   inactivity 180 s) — the loader writes the complete image.
6. The FM-1 restarts : check the `FM-1_xxx` identity of the packet (40 s).

If the FM-1 is already in loader mode (interrupted update), go directly to
step 5 (resume). Never write on a loader that is not `ota-FM-1_9XX` (loader
from another firmware : finish with its own installer).

### pack7 / unpack7

7-bit big-endian bitstream encoding (LSB first) : each output byte carries
7 input bits. `unpack7(pack7(x)) == x`. On the hub side (JS) and on the C side
(`fw_up_pack7`/`fw_up_unpack7`), verified bit by bit by the host test
`companion/tests/test_fwsc.py`.

## `.fwsc` packet (Felucca format)

Structure read in `fm1_rescue.py` (`logical_image`, `app_area`) :

- The `FELUCCA-LOADER-1` marker present in the packet : it is a hub packet.
- 20 identity bytes, one after each of the first 20 blocks of `0x2F` bytes. Product identity : `(byte - i - 1) & 0xFF` for block `i`, `0x7D` marking absence.
- `0x40`-byte ENC-encrypted header : CRC-16/XMODEM over `[2..0x40]`, entry count (u16 LE) at offset 8.
- File list : `count × 0x50` ENC-encrypted bytes. Entry : type (u16 LE), file crc (u16 LE at 4), offset (u32 LE at 8), size (u32 LE at 12). Type `0` = `flash.bin`.
- App zone in `flash.bin` : `[0x4000, 0x93000)` (see `protocol-rescue.md`).
- Catalog shortcut : product identity in `FM-1_975` format (`product`), `ident` is a matching regex.

The official M-VAVE V15 is not in Felucca format (no loader marker).
It is installed through the hub page, which serves the M-VAVE file verified
byte by byte (SHA-256 `db1642b2…` pinned, size 699 956, product `FM-1_015`).
A rescue is done first with a hub firmware, then V15 from the page.

## Remote catalog (for `firmware_hub.py`)

`GET https://thefirmwarehub.com/catalog.json`

Structure measured on 2026-11-23 :

```json
{
  "license": { "license": "CC-BY-4.0", ... },
  "updated": "2026-11-23 18:02 UTC",
  "firmwares": [
    {
      "id": "hortator",
      "name": "Hortator",
      "author": "DEADACTIVE",
      "repo": "deadactive/hortator",
      "version": "1.0.1",
      "pkg": "fw/hortator-1.0.1.fwsc",
      "size": 609649,
      "sha256": "ac81af292e035ee0c4b8b7ab3a78b942e4f03deee31ae213a2613c743cd9110f",
      "product": "FM-1_900",
      "chip": "new",
      "lic": "GPL-3.0",
      "beta": false,
      "installs": 894
    }
  ]
}
```

- `pkg` is relative to the hub root (`fw/<id>-<version>.fwsc`), absolute (hosted on GitHub) for link-only entries.
- `chip` : `old` (flash `0x856014`) or `new` (`0xB36014`, units sold since fall 2026). A firmware that only knows the old chip keeps the saves in RAM on the new units.
- Catalog license : CC-BY-4.0, hub attribution, read at most once a day.

## Security

Verify the SHA-256 of the downloaded packet against the catalog before any
send to the FM-1 (the hub does the same verification before offering a packet).
Nothing goes to a server during the flash : the packets are served by the hub,
mirrors intact of the GitHub releases.