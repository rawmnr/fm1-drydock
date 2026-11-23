# MORNING_REPORT — FM1-Drydock v0.1 (run de nuit)

Cible : v0.1 compilée, testée, flashée sur `COM6`. Régles respectées : zéro écriture sur le FM-1, aucun blocage physique, commits atomiques par jalon.

## Commits de la nuit

| Sha | Contenu |
|---|---|
| `e9c4170` | scaffold repo, spec, schema, docs probe |
| `2568607` | companion `drydock-sync` (fetch/list/sync/verify) |
| `1ccd4d6` | companion `doctor` + gates ruff/mypy + CI |
| `022f2fe` | firmware : boot COM6 validé (révision V2, AXP2101, SD) |
| `c72a787` | proto : framer C `.fwsc` bit-à-bit + test hôte |
| `222af2b` | proto : fix ruff E501 |
| `5218f18` | firmware : `drydock_storage` + `boot_mode` |

## État des jalons

| Jalon | Statut | Détail |
|---|---|---|
| 1 Socle HW/Stockage | **PARTIEL** | Boot COM6 validé sur matière (révision **V2** CO5300+CST816, PSRAM 8 Mo, display+touch, AXP2101 batt%/charge/VBUS). `drydock_storage` compile et monte/démonte (sans carte → `ESP_ERR_TIMEOUT` géré). Parseur cJSON + SHA-256 streaming mbedtls écrits, non exécutés sur carte (aucune microSD branchée). |
| 2 Dual-Role / PC Sync | **PARTIEL** | `boot_mode.h` (flag RTC_NOINIT magic+CRC, HOST_FLASH/PC_SYNC) écrit et compilé. TinyUSB MSC **non implémenté** (voir blocages). |
| 3 Flash/Rescue | **PARTIEL** | Framer C `.fwsc` (crcc16/enc/logical_image/app_area) porté de `fm1_rescue.py` + test hôte bit-à-bit. `drydock_usb_midi_host` / `drydock_usb_rescue_host` non écrits. |
| 4 UI LVGL 9 | **PARTIEL** | Écran unique de statut boot (titre + états) fonctionnel. Les 7 écrans (liste, détail, flash, rescue, sync, diag) non implémentés. |
| 5 Durcissement/CI/Rapport | **FAIT** | `drydock doctor`, ruff/mypy/strict, pytest 43 verts, CI (companion + firmware), timeout toolchain install, ESP laissé en `HOST_FLASH`. |

## Empreinte & tests

- `idf.py size` : image totale **769 031 octets** (~751 Ko), partition app 8 M **91 % libre**, RAM ≥ 115 Ko internes / 7,6 Mo PSRAM libres au boot.
- `pytest`: **43 passed, 2 skipped** (test C hôte skipé localement : aucun compilateur C hôte sur ce poste Windows ; vert sur CI Linux où `gcc` existe).
- `ruff` et `mypy --strict`: zéro erreur.
- Variante `SMOKE_USB_HOST` (probe USB Diag) : **compile** (`idf.py -DSMOKE_USB_HOST=1 build` → exit 0).

## Protocole de test matinal (≈3 min) — liaison USB-C ↔ USB-C

La carte porte le firmware **HOST_FLASH par défaut** (statut boot). Pour le test USB Host :

1. Flasher le probe USB Diag (le dossier `build/` contient déjà cette variante) :
   ```
   cd firmware
   idf.py -p COM6 flash
   ```
   (s'il a été reconstruit en défaut : `idf.py -DSMOKE_USB_HOST=1 build` puis `flash`).
2. Débrancher la carte du PC. L'alimenter (batterie MX1.25 recommandée ; la carte seule sans 5 V ne peut pas piloter VBUS Host).
3. Relier le **FM-1 à l'USB-C de la carte** (câble USB-C ↔ USB-C).
4. Lire l'AMOLED : l'écran doit afficher **`USB Host: VID=4D4A PID=4155`** quand le FM-1 s'énumère (status USB: débranché sinon).

Point matériel risqué (à trancher au test) : le FM-1 est auto-alimenté, mais il faut que la carte fournisse du 5 V sur VBUS en mode Host pour l'énumération. Si rien ne s'énumère, vérifier l'alimentation de la carte et la présence VBUS (c'est LE point non résolu de la Phase 0, noté dans `docs/hardware-waveshare-1.8.md`).

## Blocages / points ouverts de la nuit

- **Aucune microSD branchée** : validation physique du montage + manifest + sha256 en attente d'une carte. Le code est prêt et compile.
- **Aucun compilateur C hôte Windows** : le test bit-à-bit `.fwsc` passe sur CI (Linux/gcc), skipé localement.
- **Tension batterie AXP2101** : ADC brut mesuré (`4112`), échelle mV **non calibrée** (datasheet) — % / charge / VBUS sont fiables, la tension est à confirmer avec le datasheet.
- **Carte révision** : **V2** confirmée (CST816 répond, FT3168 absent). Les écrans/UI devront cibler le CO5300.
- **Host/Device même port USB-C** : la console série et le mode Host partagent les broches ; le probe Host est donc une build séparée (`SMOKE_USB_HOST`), pas le défaut (qui doit rester branchable au PC).

## Corrections post-review (reviewer indépendant)

Une relecture indépendante du trail a relevé 7 points ; tous corrigés et commités (`b1a1f39`, `891740e`) :

- **`fwsc.c` use-after-free** : `out->app` pointait dans un tampon libéré. Le tampon est maintenant `owned` par `fwsc_info_t` et libéré via `fwsc_free()`.
- **Test `.fwsc` skipé même en CI** : il n'y avait aucun `.fwsc` en cache sur le runner. Le test télécharge désormais des fixtures réelles depuis le hub quand elles sont absentes → il s'exécute vraiment en CI (Linux/gcc) et localement avec un compilateur C.
- **SHA-256 streaming** : échec → `ferror()` vérifié avant de finaliser le digest (un préfixe corrompu n'est plus rendu valide).
- **Validation manifest** : `generated_at`, `rescue_file` et chaque vessel (`file`+`sha256`) sont maintenant validés.
- **Cache GitHub** : l'asset d'une release est toujours re-téléchargé (plus de risque de servir l'ancienne version sous un nouveau numéro).
- **Télémétrie AXP2101** : une lecture I2C ratée affiche `n/a` au lieu de zéros trompeurs.

Le firmware durci a été reflashé sur `COM6` et re-vérifié par log série : boot `HOST_FLASH`, révision V2, AXP OK, display OK, SD sans carte gérée, boucle d'`alive` stable.