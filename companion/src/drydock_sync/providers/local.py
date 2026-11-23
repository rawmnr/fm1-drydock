"""Sources .fwsc locales (firmwares hors hub)."""
from __future__ import annotations

from pathlib import Path


def load(path: Path) -> bytes:
    if not path.is_file():
        raise FileNotFoundError(f"{path}: fichier local absent")
    return path.read_bytes()
