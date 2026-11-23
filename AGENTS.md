# AGENTS.md — FM1-Drydock

## Environnement Hardware-in-the-Loop (HIL)
Les deux périphériques sont connectés en USB au poste de développement :
1. **Waveshare ESP32-S3-Touch-AMOLED-1.8** (port série/JTAG USB-C).
2. **M-VAVE FM-1** (périphérique USB-MIDI en mode normal, ou USB-MSC `UBOOT1.00` en mode bootloader).

## Règles de sécurité matérielle (STRICTES)
- **Ne jamais écrire sur la flash du M-VAVE FM-1** sans confirmation explicite de l'utilisateur.
- Les opérations en **lecture seule** sur le FM-1 (inspection des descripteurs USB, énumération MIDI, `fm1_rescue.py --check`, dump de sauvegarde) sont autorisées.
- Avant tout flash sur l'ESP32-S3, vérifier le port COM/tty cible pour ne pas interférer avec le FM-1.

## Conventions techniques
- **Python (`companion/`)** : utiliser exclusivement `uv` (`uv run`, `uv add`, `uv run pytest`). Validation stricte via Pydantic v2.
- **Firmware (`firmware/`)** : ESP-IDF ≥ 5.5. Ne pas utiliser le framework Arduino.
- **Contrat partagé** : toute modification de la structure de `manifest.json` doit être répercutée simultanément dans `schema/manifest.schema.json`, `companion/src/drydock_sync/models.py` et `firmware/components/drydock_storage/`.

## Ordre d'exécution recommandé pour démarrer
1. **Scaffold** : créer l'arborescence complète décrite dans `SPEC.md` et `schema/manifest.schema.json`.
2. **Probe matériel** :
   - Lister les périphériques USB connectés au PC pour relever le port de l'ESP32-S3 et les VID/PID + interfaces USB du FM-1.
   - Documenter les résultats dans `docs/protocol-sysex.md` et `docs/hardware-waveshare-1.8.md`.
3. **Probe distant** : inspecter l'API / le catalogue JSON de `https://thefirmwarehub.com/` et le script `https://thefirmwarehub.com/rescue/fm1_rescue.py` pour implémenter `companion/src/drydock_sync/providers/firmware_hub.py`.
4. **Validation incrémentale** : écrire les tests unitaires côté `companion/` avant d'attaquer la pile USB Host côté `firmware/`.
