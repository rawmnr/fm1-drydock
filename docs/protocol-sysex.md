# Flash FM-1 en mode normal (USB-MIDI SysEx)

Sources : probe PC du 2026-11-23, script Web MIDI de `thefirmwarehub.com`
(mesuré le 2026-11-24), `fm1_rescue.py`, catalogue du hub.

## Identité mesurée sur USB

FM-1 en mode normal : `USB\VID_4D4A&PID_4155`, classe MEDIA (périphérique USB-MIDI),
interface MI_00 d'un composite. Le protocole de mise à jour est **M-UPGRADE** :
il passe par le port MIDI standard (SysEx), donc par le même chemin que le Web
MIDI de la page du hub (Chrome/Edge).

## Protocole de mise à jour M-UPGRADE (mesuré dans le JS du hub)

Le portage C vit dans `firmware/components/drydock_proto/upgrade.{c,h}` et le
moteur USB Host dans `firmware/components/drydock_usb_midi_host/`.

### Poignée de main (identité)

Envoyer `HS_QUERY` :

```
F0 00 32 45 00 00 00 40 7F F7
```

Le FM-1 répond par un SysEx dont le corps dépaquetté (pack7) fait 34 octets,
`00 59 11` en tête, texte aux octets 6..32 (NUL-terminé) : l'identité du
firmware, format `NAME_ddd` (ex. `FM-1_900`, `SLOOP_26`). Le loader de mise à
jour répond `ota-FM-1_9XX` (`IS_OTA` dans le hub).

### Touches

| Touche | SysEx | Effet |
|---|---|---|
| `UPGRADE` | `F0 22 24 35 7F F7` | firmware : met en place le loader ; loader : lance l'écriture |
| `UBOOT_KEY` | `F0 22 24 35 7D F7` | bascule en mode rescue `UBOOT1.00` (voir `protocol-rescue.md`) |

Ne pas confondre les deux (0x7F vs 0x7D).

### Paquets de lecture (cmd 0x30)

Pendant une mise à jour le FM-1 envoie des requêtes de lecture sur **l'image
logique** (le `.fwsc` sans les 20 octets marqueurs d'identité). Chaque requête
est un SysEx dont le corps dépaquetté (pack7) fait **15 octets** :

```
00 59 30 xx xx 00 fl a3 a2 a1 a0 l2 l1 l0 ck
```

- `addr` (octets 7..10, LE32) et `len` (octets 11..13, LE24) désignent la
  plage lue dans l'image logique.
- `ck = ~sum(octets 6..13) & 0xFF` (checksum 8 bits).
- Adresses spéciales : `0xE0000000` = fin d'étape 1 (le firmware a mis en
  place le loader), `0xF0000000` = fin d'étape 2 (écriture terminée).

### Réponse

Corps dépaquetté `14 + len + 1` octets :

```
00 59 30 (len+8)lo (len+8)hi 00 fl a3 a2 a1 a0 llo lhi 00 data... ck
```

`ck = ~sum(octets 6..fin de data) & 0xFF`. Le tout est pack7 et enveloppé
`F0 .. F7`. `len` max : 512 octets (`MAXDATA`). Aux adresses de fin, répondre
`"success"` (7 octets + checksum).

### Séquence d'installation (identique au hub)

1. Poignée de main : identité non-OTA ; vérifier que le modèle du paquet
   correspond (`FM-1_xxx` → FM-1).
2. Envoyer `UPGRADE`, attendre ~2 s.
3. Sertir les lectures (fin `0xE0000000`, inactivité 8 s) — le firmware
   vérifie le paquet et met en place le loader.
4. Le FM-1 redémarre en loader : attendre l'identité `ota-FM-1_9XX` (30 s).
5. Envoyer `UPGRADE`, attendre ~2 s, sertir les lectures (fin `0xF0000000`,
   inactivité 180 s) — le loader écrit l'image complète.
6. Le FM-1 redémarre : vérifier l'identité `FM-1_xxx` du paquet (40 s).

Si le FM-1 est déjà en mode loader (mise à jour coupée), passer directement à
l'étape 5 (resume). Ne jamais écrire sur un loader qui n'est pas
`ota-FM-1_9XX` (loader d'un autre firmware : finir avec son propre installeur).

### pack7 / unpack7

Encodage 7 bits big-endian bitstream (LSB first) : chaque octet de sortie
porte 7 bits d'entrée. `unpack7(pack7(x)) == x`. Côté hub (JS) et côté C
(`fw_up_pack7`/`fw_up_unpack7`), vérifiés bit à bit par le test hôte
`companion/tests/test_fwsc.py`.

## Paquet `.fwsc` (format Felucca)

Structure lue dans `fm1_rescue.py` (`logical_image`, `app_area`) :

- Marqueur `FELUCCA-LOADER-1` présent dans le paquet : c'est un paquet hub.
- 20 octets d'identité, un après chacun des 20 premiers blocs de `0x2F` octets. Identité produit : `(octet - i - 1) & 0xFF` pour le bloc `i`, `0x7D` marque l'absence.
- En-tête `0x40` octets chiffré ENC : CRC-16/XMODEM sur `[2..0x40]`, nombre d'entrées (u16 LE) en offset 8.
- Liste de fichiers : `count × 0x50` octets chiffrés ENC. Entrée : type (u16 LE), crc du fichier (u16 LE à 4), offset (u32 LE à 8), taille (u32 LE à 12). Type `0` = `flash.bin`.
- Zone d'app dans `flash.bin` : `[0x4000, 0x93000)` (voir `protocol-rescue.md`).
- Raccourci catalogue : identité produit au format `FM-1_975` (`product`), `ident` est une regex de correspondance.

Le V15 officiel M-VAVE n'est pas au format Felucca (pas de marqueur de loader).
Il s'installe par la page du hub, qui sert le fichier M-VAVE vérifié octet par
octet (SHA-256 `db1642b2…` épinglé, taille 699 956, produit `FM-1_015`). Un
rescue se fait d'abord avec un firmware hub, puis V15 depuis la page.

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
