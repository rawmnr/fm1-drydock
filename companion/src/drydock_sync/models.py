"""Modèles Pydantic alignés sur schema/manifest.schema.json.

Toute modification de la structure de manifest.json doit être répercutée
simultanément dans schema/manifest.schema.json, ce fichier et
firmware/components/drydock_storage/ (règle AGENTS.md).
"""
from __future__ import annotations

import tomllib
from datetime import datetime
from enum import StrEnum
from pathlib import Path
from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field, model_validator

Sha256 = Annotated[str, Field(pattern=r"^[0-9a-f]{64}$")]


class Source(StrEnum):
    """Origine d'un firmware dans vessels.toml."""

    firmware_hub = "firmware_hub"
    github = "github"
    local = "local"


class FileEntry(BaseModel):
    """Une entrée du manifest : stock_v15 ou un élément de vessels."""

    model_config = ConfigDict(extra="forbid")

    id: str = Field(min_length=1)
    name: str = Field(min_length=1)
    version: str | None = None
    author: str | None = None
    category: str | None = None
    description: str | None = None
    source: Source | None = None
    file: str = Field(pattern=r"^(stock|vessels)/[A-Za-z0-9._-]+$")
    size_bytes: int = Field(default=0, ge=0)
    sha256: Sha256


class RescueHelper(BaseModel):
    """Helper loader JieLi (wl82loader.bin), stocké sous rescue/."""

    model_config = ConfigDict(extra="forbid")

    file: str = Field(pattern=r"^rescue/[A-Za-z0-9._-]+$")
    sha256: Sha256


class Manifest(BaseModel):
    """/sdcard/drydock/manifest.json, le contrat partagé PC <-> ESP32."""

    model_config = ConfigDict(extra="forbid")

    schema_version: Literal[1] = 1
    generated_at: datetime
    stock_v15: FileEntry
    rescue_helper: RescueHelper
    vessels: list[FileEntry] = Field(min_length=1)

    def to_json_bytes(self) -> bytes:
        return self.model_dump_json(indent=2, exclude_none=True).encode("utf-8") + b"\n"


class VesselConfig(BaseModel):
    """Une entrée [[vessel]] de vessels.toml."""

    model_config = ConfigDict(extra="forbid")

    id: str = Field(min_length=1, pattern=r"^[A-Za-z0-9._-]+$")
    name: str | None = None
    source: Source
    repo: str | None = None
    asset: str | None = None
    path: str | None = None

    @model_validator(mode="after")
    def _check_source_fields(self) -> VesselConfig:
        if self.source is Source.github and not (self.repo and self.asset):
            raise ValueError(f"vessel '{self.id}': source github requires 'repo' and 'asset'")
        if self.source is Source.local and not self.path:
            raise ValueError(f"vessel '{self.id}': source local requires 'path'")
        return self


class VesselsConfig(BaseModel):
    """Contenu de vessels.toml : une liste [[vessel]] à la racine."""

    model_config = ConfigDict(extra="forbid")

    vessel: list[VesselConfig] = Field(min_length=1)


def load_vessels(path: Path) -> VesselsConfig:
    """Parse un fichier vessels.toml et valide les contraintes par source."""
    with path.open("rb") as f:
        return VesselsConfig.model_validate(tomllib.load(f))
