"""Pydantic models aligned with schema/manifest.schema.json.

Any change to the manifest.json structure must be mirrored
simultaneously in schema/manifest.schema.json, this file and
firmware/components/drydock_storage/ (AGENTS.md rule).
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
    """Origin of a firmware in vessels.toml."""

    firmware_hub = "firmware_hub"
    github = "github"
    local = "local"


class FileEntry(BaseModel):
    """A manifest entry: stock_v15 or a vessels element."""

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
    """JieLi loader helper (wl82loader.bin), stored under rescue/."""

    model_config = ConfigDict(extra="forbid")

    file: str = Field(pattern=r"^rescue/[A-Za-z0-9._-]+$")
    sha256: Sha256


class Manifest(BaseModel):
    """/drydock/manifest.json (internal FAT partition), the shared PC <-> ESP32 contract."""

    model_config = ConfigDict(extra="forbid")

    schema_version: Literal[1] = 1
    generated_at: datetime
    stock_v15: FileEntry
    rescue_helper: RescueHelper
    vessels: list[FileEntry] = Field(min_length=1)

    def to_json_bytes(self) -> bytes:
        return self.model_dump_json(indent=2, exclude_none=True).encode("utf-8") + b"\n"


class VesselConfig(BaseModel):
    """A [[vessel]] entry of vessels.toml."""

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
    """Contents of vessels.toml: a top-level [[vessel]] list."""

    model_config = ConfigDict(extra="forbid")

    vessel: list[VesselConfig] = Field(min_length=1)


def load_vessels(path: Path) -> VesselsConfig:
    """Parse a vessels.toml file and validate constraints by source."""
    with path.open("rb") as f:
        return VesselsConfig.model_validate(tomllib.load(f))
