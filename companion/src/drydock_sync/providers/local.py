"""Local .fwsc sources (firmwares outside the hub)."""
from __future__ import annotations

from pathlib import Path


def load(path: Path) -> bytes:
    if not path.is_file():
        raise FileNotFoundError(f"{path}: local file missing")
    return path.read_bytes()
