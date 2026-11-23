# Rescue JieLi (UBOOT1.00 / SCSI)

Référence : `fm1_rescue.py` (GPL-3.0) et `jl-uboot-tool` (MIT, kagaimiq). Portage du FLASH rescue de l'ESP32-S3 : `drydock_usb_rescue_host`.

## Quand

Un FM-1 que plus aucun installer ne trouve, ou une mise à jour coupée. Le bootloader est dans la ROM du chip AC791N (mask ROM) : aucun firmware ne peut l'écraser.

## Entrée en bootloader

1. Via USB-MIDI : envoyer `F0 22 24 35 7D F7` (bouton « Send to the bootloader » de la page hub, Chrome/Edge). Nécessite un firmware qui tourne encore.
2. À l'allumage : maintenir OCT− pendant la mise sous tension (FAQ hub, « USB rescue »).

Résultat : le FM-1 s'énumère en disque USB. Vendor `WL82`, modèle `UBOOT1.00`. Rien n'est écrit ; couper l'alimentation restore le firmware d'origine.

## Transport

SCSI pass-through. Linux : `/dev/sg*` via l'ioctl SG_IO. Windows et Mac : `MvaveFM1Unbricker` (Quixotic7, MIT) emprunte le même chemin. Le portage ESP32-S3 utilise `usb_host_msc` en Host.

## Jeu de commandes

CDB de 16 octets : `op`, `sub`, arguments, puis `0xFF` jusqu'à 16. Protocole v2.

| Opération | op | sub | Arguments | Payload |
|---|---|---|---|---|
| Lire mémoire RAM | `0xFD` | `0x07` | addr BE32, n BE16 | sortie n octets |
| Écrire mémoire RAM | `0xFB` | `0x06` | addr BE32, len BE16, `0x00`, crc16 LE16 | données |
| Sauter mémoire | `0xFB` | `0x08` | addr BE32, arg BE16 | sortie 16 octets |
| Lire id flash | `0xFC` | `0x0B` | — | 16 octets, réponse décalée d'un octet (`00 85 60 ...`) |
| Lire flash | `0xFD` | `0x05` | addr BE32, n BE16 | données |
| Effacer secteur | `0xFB` | `0x01` | addr BE32 | — (timeout 30 s) |
| Écrire flash | `0xFB` | `0x04` | addr BE32, len BE16, `0x00`, crc16 LE16 | données |
| Lancer l'app | `0xFC` | `0x0C` | arg BE32 = 1 | sortie 16 octets |

## Ciphers JieLi

- CRC-16/XMODEM, poly `0x1021` : paquet, records flash, bootloader.
- ENC : chiffre de flux (xor + rétroaction LFSR poly `0x1021`).
- SFC : ENC par blocs de 32 octets, clé xorée avec `offset >> 2`.
- MengLi : chiffre CRC du ROM pour les accès mémoire RAM (`crc16` chaîné avec la magie GB2312 `孟黎我爱你，玉林`). Le ROM répond chiffré MengLi ; le helper chargé répond en clair.

## Helper loader

- Fichier : `wl82loader.bin`, commit `adb3f18889e88ac512ce0a3c4d8cc3d3cb30696a` de `jl-uboot-tool`.
- SHA-256 : `d41da6126760c9d66660bcc0cac8d27d221806c5e369a8036921efe68dca5376`.
- Chargé en RAM `0x01C02000`, argument `0x0001` (SPI NOR). Transmis par blocs de 512 octets chiffrés MengLi.
- Reprise : si `mem_read(JL_ADDR, 16)` renvoie les octets en clair, le helper tourne déjà, ne pas le réécrire.

## Flash

- Chip attendu : JEDEC `0x8560` (Puya, anciennes unités) ou `0xB360` (UC25HQ80, unités récentes).
- Layout : tête `0x0000..0x3FFF` jamais écrite (SPL, clé chip, entrées JLFS). Zone d'app `0x4000..0x92FFF`. Secteurs de 4 Ko.
- Écriture : uniquement les secteurs qui diffèrent. Par secteur : erase, écriture par tranches de 256 octets, read-back. Trois essais. Reprise possible si la session est coupée. Vérification finale complète, puis `run_app`.

## Clé chip

Lue depuis la tête du FM-1 lui-même, jamais depuis le paquet :

- Entrée JLFS `isd_config.ini` (32 octets chiffrés ENC, CRC16) pointe un blob de 34 octets (32 + CRC16).
- Dérivation : `s = sum(blob[:16]) & 0xFF`, borné à `0x55` en dessous de `0x10` et `0xAA` au-dessus de `0xE0`. Bit `i` = 1 si `(blob[16+i] ^ blob[15-i]) < s`.

Un paquet ne s'applique que si son zone d'app se décrypte avec cette clé (`sfc` + CRC16 du premier bloc).

## Invariants pour le portage

1. Ne jamais écrire la tête du flash.
2. Vérifier avant d'écrire, dans l'ordre : chip flash, clé chip, paquet. Sinon arrêter sans écrire.
3. Chaque secteur écrit est relu. Échec = stop, reprise possible au prochain run.
4. Garder un mode lecture seule (équivalent `--check`) : lire, comparer, ne rien écrire.
5. Exiger une confirmation explicite avant d'écrire.
