# MORNING_REPORT — FM1-Drydock v0.2 (run du 2026-11-24)

Cible : v0.2 complète — stockage interne, PC Sync MSC, USB Host MIDI/Rescue,
UI LVGL 9 — compilée et validée au maximum sans accès physique. Règles
respectées : zéro écriture sur le FM-1, commits atomiques par jalon.

## Commits de la nuit

| Sha | Contenu |
|---|---|
| `2172c87` | **Jalon 1** : partition FAT interne 12 Mo (`drydock_fs`, Wear Levelling) à la place de la microSD, VFS sur `/drydock`, layout racine |
| `6e412db` | **Jalon 2** : mode DEVICE_PC_SYNC — TinyUSB MSC spiflash (volume `DRYDOCK`), boot dual-mode, console `pc_sync`/`host`, bouton DONE & Reboot, auto-retour ~4 s après débranchement/éjection |
| `7d688c6` | **Jalon 3** : moteurs USB Host — `drydock_usb_midi_host` (M-UPGRADE : handshake, cmd 0x30, touches UPGRADE/UBOOT), `drydock_usb_rescue_host` (BOT/SCSI, helper loader, clé chip, flash sectoriel), `drydock_proto` (fwsc/jielib/upgrade, test hôte bit-à-bit), `drydock_usb_host` (lib partagée) |
| `e88cc6c` | **Jalon 4** : UI LVGL 9 — accueil (liste vessels), détail+flash (SHA-256, progression, Stock V15 first), Rescue & Stock, PC Sync, USB Diag, barre de statut (batterie/USB/espace) |

## État des jalons

| Jalon | Statut | Détail |
|---|---|---|
| 1 Stockage interne | **FAIT** | Partition `drydock_fs` 12 Mo montée en FAT+WL sur `/drydock` (`free 12 431 360 octets`), manifest parse, premier boot propre sans microSD. |
| 2 PC Sync MSC | **FAIT (HIL partiel)** | HIL #2 : volume `DRYDOCK` (D:\), `drydock probe/sync/verify` **tous OK** sur matière (stock + rescue + 3 vessels + manifest < 2 Mo). Auto-retour HOST_FLASH testé en éjection SCSI. |
| 3 Flash/Rescue USB Host | **FAIT (code + tests hôte)** | M-UPGRADE complet (identité, verify → loader → write → reboot) et Rescue UBOOT1.00 (check lecture seule + flash confirmé). Protocole mesuré dans le JS du hub (`docs/protocol-sysex.md`). Test hôte bit-à-bit C ↔ Python (CI gcc). |
| 4 UI LVGL 9 | **FAIT (compile)** | 6 écrans + barre de statut, callbacks flash → UI sous lock display. Non exécutée sur carte (board bloquée en MSC, voir blocages). |
| 5 Durcissement/CI | **FAIT** | pytest **43 passed, 10 skipped** (C hôte, pas de gcc Windows), ruff/mypy stricts, `idf.py size` image **861 135 octets** (~841 Ko), partition 3 M, build zéro warning. |

## Blocages / points ouverts

- **La carte est restée en mode PC_SYNC (MSC)** : le test HIL complet (retour
  HOST_FLASH, parse manifest, `verify_sha256_streaming` bit-à-bit) et le flash
  du firmware final n'ont **pas pu s'exécuter**. Windows (session non-admin)
  refuse de retirer le nœud du périphérique (« not removable »), donc
  `tud_connected()` reste vrai côté carte. **Déblocage = débrancher/rebrancher
  la carte (ou un admin : `pnputil /remove-device "USB\VID_303A&PID_4002\0001"`)**.
  Le firmware in-tree contient déjà le fix `is_exposed` : dès le prochain flash,
  une éjection SCSI suffit à déclencher l'auto-retour (plus besoin de débrancher).
- **Aucun compilateur C hôte Windows** : test bit-à-bit C `.fwsc`/M-UPGRADE skipé
  localement, vert sur CI (Linux/gcc).
- **VBUS Host** : le FM-1 doit être alimenté par la carte en mode Host
  (point risqué Phase 0, `docs/hardware-waveshare-1.8.md`) — à valider au
  prochain HIL.
- **Flash MIDI non testé sur matière** : le protocole est porté bit-à-bit du
  JS du hub et vérifié par le test hôte, mais l'exécution réelle sur le FM-1
  attend le déblocage de la carte.
- **Interface FM-1 mesurée (PnP PC, lecture seule)** : composite
  `VID_4D4A&PID_4155`, MI_00 = AudioControl `Class_01&SubClass_01` (sans
  endpoint, « MEDIA » sous Windows) ; l'interface MIDI streaming
  `0x01/0x03` n'apparaît pas sous Windows mais existe dans le descripteur.
  Le host cherche donc une interface **avec endpoints IN+OUT**
  (`drydock_usb_find_data_iface`, fix `5f96058`).

## Protocole de test matinal (≈5 min) — après débranchement/rebranchement

1. **Récupérer la carte** : débrancher/rebrancher l'USB-C de la carte (le flag
   RTC_NOINIT est volatil : un power cycle repasse en `HOST_FLASH`, COM6 revient).
2. **Flasher le firmware final** (celui de `e88cc6c`, UI + moteurs) :
   ```
   cd firmware
   idf.py -p COM6 flash monitor
   ```
3. **Vérifier le boot** (30 s de log) : révision V2, AXP2101, FAT monté
   (`libre ~12 Mo`), manifest OK (n vessels), console `drydock>`, pas de
   crash/watchdog. L'AMOLED affiche la liste des vessels + barre de statut.
4. **Boucle PC Sync complète** :
   - bouton **PC SYNC** sur l'écran (ou console `pc_sync`) → redémarre en MSC ;
   - le PC voit le volume `DRYDOCK` : `drydock probe`, `drydock sync`,
     `drydock verify` (déjà validés, re-jouer pour confirmer) ;
   - éjecter le volume : la carte doit **revenir seule en HOST_FLASH** (~4 s)
     grâce au fix `is_exposed` ; vérifier le log « hôte USB parti ».
5. **USB Host (batterie requise ou 5 V sur le port)** : débrancher la carte du
   PC, relier le **FM-1 à l'USB-C de la carte** (câble USB-C ↔ USB-C).
   - écran **USB Diag** : `FM-1 (MIDI)`, identité `FM-1_xxx` lue ;
   - flasher un vessel depuis l'UI (bouton FLASH) : progression, statut final ;
   - **Rescue** : bouton Enter Bootloader → le FM-1 passe en `UBOOT1.00`,
     Rescue check lit id flash + clé chip + secteurs à écrire, sans rien écrire.
6. **Retour Stock V15** : bouton « Back to Stock V15 » → vérifier l'identité
   `FM-1_015` au reboot.

## Notes techniques

- **Nouveaux composants** : `drydock_proto` (fwsc/jielib/upgrade, pur C testé
  hôte), `drydock_usb_host` (usb_host_lib partagée), `drydock_usb_midi_host`,
  `drydock_usb_rescue_host`, `drydock_ui`, `drydock_boot` (boot_mode extrait).
- **Layout `/drydock/`** : fichiers à la racine (plus de sous-dossier
  `drydock/`) — un ancien flash avec sous-dossier montrerait une dualité, une
  re-synchronisation est requise (déjà faite au HIL #2).
- **Console** : USB-SERIAL-JTAG (COM6), commandes `pc_sync` / `host`.
- **`CONFIG_TINYUSB_MSC_BUFSIZE=4096`** : ne pas descendre (≥ WL_SECTOR_SIZE).
