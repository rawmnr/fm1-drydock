# Flash FM-1 en mode normal (USB-MIDI SysEx)

Sources : probe PC du 2026-11-23, `fm1_rescue.py`, page et catalogue de `thefirmwarehub.com`.

## Identité mesurée sur USB

FM-1 en mode normal : `USB\VID_4D4A&PID_4155`, classe MEDIA (périphérique USB-MIDI), interface MI_00 d'un composite. VID `0x4D4A`, PID `0x4155`.

[À vérifier Phase 0] Détail des endpoints MIDI (buffers, taille des SysEx). Windows PnP ne l'expose pas ; à relever sous Linux (`lsusb -v`) ou via Web MIDI.

## Principe de la mise à jour

Le hub parle au FM-1 en USB-MIDI avec le protocole de mise à jour de M-VAVE : le firmware en cours d'exécution met d'abord en place le loader de mise à jour du paquet, puis ce loader écrit le firmware. Source : FAQ du hub, « What happens when you press Install ».

Envoi au bootloader : séquence SysEx `F0 22 24 35 7D F7` sur le port MIDI du FM-1 (clé `UBOOT_KEY` de `fm1_rescue.py`). L'écran s'éteint, le FM-1 s'énumère en disque USB `UBOOT1.00` (voir `protocol-rescue.md`).

## Paquet `.fwsc` (format Felucca)

Structure lue dans `fm1_rescue.py` (`logical_image`, `app_area`) :

- Marqueur `FELUCCA-LOADER-1` présent dans le paquet : c'est un paquet hub.
- 20 octets d'identité, un après chacun des 20 premiers blocs de `0x2F` octets. Identité produit : `(octet - i - 1) & 0xFF` pour le bloc `i`, `0x7D` marque l'absence.
- En-tête `0x40` octets chiffré ENC : CRC-16/XMODEM sur `[2..0x40]`, nombre d'entrées (u16 LE) en offset 8.
- Liste de fichiers : `count × 0x50` octets chiffrés ENC. Entrée : type (u16 LE), crc du fichier (u16 LE à 4), offset (u32 LE à 8), taille (u32 LE à 12). Type `0` = `flash.bin`.
- Zone d'app dans `flash.bin` : `[0x4000, 0x93000)` (voir `protocol-rescue.md`).
- Raccourci catalogue : identité produit au format `FM-1_975` (`product`), `ident` est une regex de correspondance.

Le V15 officiel M-VAVE n'est pas au format Felucca (pas de marqueur de loader). Il s'installe par la page du hub, qui sert le fichier M-VAVE vérifié octet par octet. Un rescue se fait d'abord avec un firmware hub, puis V15 depuis la page.

## Catalogue distant (pour `firmware_hub.py`)

`GET https://thefirmwarehub.com/catalog.json`

Structure mesurée le 2026-11-23 :

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

- `pkg` est relatif à la racine du hub (`fw/<id>-<version>.fwsc`), absolu (hébergé GitHub) pour les fiches link-only.
- `chip` : `old` (flash `0x856014`) ou `new` (`0xB36014`, unités vendues depuis automne 2026). Un firmware qui ne connaît que l'ancien chip garde les sauvegardes en RAM sur les nouvelles unités.
- Licence du catalogue : CC-BY-4.0, attribution du hub, lire au plus une fois par jour.

## Sécurité

Vérifier le SHA-256 du paquet téléchargé contre le catalogue avant tout envoi au FM-1 (le hub fait la même vérification avant d'offrir un paquet). Rien ne part vers un serveur au flash : les paquets sont servis par le hub, miroirs intacts des releases GitHub.
