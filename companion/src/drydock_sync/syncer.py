"""Orchestration fetch/sync : cache local, diff SHA-256, layout SD, manifest atomique."""
from __future__ import annotations

import hashlib
import os
from collections.abc import Callable
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from .models import FileEntry, Manifest, RescueHelper, Source, VesselConfig, VesselsConfig
from .providers import local as local_provider
from .providers.firmware_hub import RESCUE_SHA256, V15_SHA256, FirmwareHub
from .providers.github import GithubProvider

STOCK_REL = "stock/fm1_v15_official.fwsc"
RESCUE_REL = "rescue/jl_flash_helper.bin"


@dataclass
class ResolvedEntry:
    """Un fichier prêt à copier sur la SD, et sa déclaration manifest."""

    id: str
    name: str
    rel_path: str  # vessels/<id>.fwsc | stock/... | rescue/...
    data: bytes
    sha256: str
    version: str | None = None
    author: str | None = None
    category: str | None = None
    description: str | None = None
    source: Source | None = None

    @property
    def file_entry(self) -> FileEntry:
        return FileEntry(
            id=self.id,
            name=self.name,
            version=self.version,
            author=self.author,
            category=self.category,
            description=self.description,
            source=self.source,
            file=self.rel_path,
            size_bytes=len(self.data),
            sha256=self.sha256,
        )


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def _atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_bytes(data)
    os.replace(tmp, path)


class Cache:
    """Cache de téléchargement, un fichier par entrée avec sidecar .sha256.

    Le sidecar sert aux sources sans hash autoritatif (github, local).
    """

    def __init__(self, root: Path):
        self.root = root

    def path_for(self, rel_path: str) -> Path:
        return self.root / rel_path

    def valid(self, rel_path: str, expected_sha: str) -> bool:
        p = self.path_for(rel_path)
        return p.is_file() and sha256_file(p) == expected_sha

    def valid_with_sidecar(self, rel_path: str) -> str | None:
        """Rend le hash d'un fichier en cache si son sidecar correspond, sinon None."""
        p = self.path_for(rel_path)
        side = self.path_for(rel_path + ".sha256")
        if not p.is_file() or not side.is_file():
            return None
        want = side.read_text().strip()
        return want if sha256_file(p) == want else None

    def ensure(self, rel_path: str, expected_sha: str | None, producer: Callable[[], bytes]) -> bytes:
        """Rend les octets du cache, sinon produit, vérifie et écrit."""
        if expected_sha is not None and self.valid(rel_path, expected_sha):
            return self.path_for(rel_path).read_bytes()
        if expected_sha is None:
            cached_sha = self.valid_with_sidecar(rel_path)
            if cached_sha is not None:
                return self.path_for(rel_path).read_bytes()
        data = producer()
        if expected_sha is not None and sha256_bytes(data) != expected_sha:
            raise ValueError(f"{rel_path}: SHA-256 du téléchargement != attendu {expected_sha}")
        self.write(rel_path, data)
        return data

    def write(self, rel_path: str, data: bytes) -> None:
        _atomic_write(self.path_for(rel_path), data)
        _atomic_write(self.path_for(rel_path + ".sha256"), sha256_bytes(data).encode() + b"\n")


class Resolver:
    def __init__(self, hub: FirmwareHub, github: GithubProvider, cache: Cache, config_dir: Path):
        self.hub = hub
        self.github = github
        self.cache = cache
        self.config_dir = config_dir

    def resolve_vessel(self, v: VesselConfig) -> ResolvedEntry:
        if v.source is Source.firmware_hub:
            return self._resolve_hub(v)
        if v.source is Source.github:
            return self._resolve_github(v)
        return self._resolve_local(v)

    def _resolve_hub(self, v: VesselConfig) -> ResolvedEntry:
        fw = self.hub.resolve(v.id)
        rel = f"vessels/{v.id}.fwsc"
        data = self.cache.ensure(rel, fw.sha256, lambda: self.hub.download_pkg(fw))
        kind = fw.kind
        category = kind if isinstance(kind, str) else ", ".join(kind) if kind else None
        return ResolvedEntry(
            id=v.id, name=v.name or fw.name, rel_path=rel, data=data, sha256=fw.sha256,
            version=fw.version, author=fw.author, category=category, description=fw.what,
            source=Source.firmware_hub,
        )

    def _resolve_github(self, v: VesselConfig) -> ResolvedEntry:
        release = self.github.latest_release(v.repo)
        asset = self.github.find_asset(release, v.asset)
        rel = f"vessels/{v.id}.fwsc"
        data = self.cache.ensure(rel, None, lambda: self.github.download_asset(asset))
        return ResolvedEntry(
            id=v.id, name=v.name or asset["name"], rel_path=rel, data=data,
            sha256=sha256_bytes(data), version=release.get("tag_name"), source=Source.github,
        )

    def _resolve_local(self, v: VesselConfig) -> ResolvedEntry:
        src = (self.config_dir / v.path).resolve()
        data = local_provider.load(src)
        rel = f"vessels/{v.id}.fwsc"
        return ResolvedEntry(
            id=v.id, name=v.name or src.name, rel_path=rel, data=data,
            sha256=sha256_bytes(data), source=Source.local,
        )

    def resolve_stock(self) -> ResolvedEntry:
        data = self.cache.ensure(STOCK_REL, V15_SHA256, self.hub.stock_v15)
        return ResolvedEntry(
            id="stock-v15", name="M-VAVE Official V15", rel_path=STOCK_REL, data=data,
            sha256=V15_SHA256, version="15",
        )

    def resolve_rescue(self) -> ResolvedEntry:
        data = self.cache.ensure(RESCUE_REL, RESCUE_SHA256, self.hub.rescue_helper)
        return ResolvedEntry(
            id="rescue-helper", name="JieLi flash helper (UBOOT1.00)", rel_path=RESCUE_REL,
            data=data, sha256=RESCUE_SHA256,
        )


def resolve_all(resolver: Resolver, cfg: VesselsConfig) -> list[ResolvedEntry]:
    """Stock V15 + helper rescue + les vessels de la config, dans cet ordre."""
    entries = [resolver.resolve_stock(), resolver.resolve_rescue()]
    entries += [resolver.resolve_vessel(v) for v in cfg.vessel]
    return entries


@dataclass
class SyncResult:
    entries: list[ResolvedEntry]
    copied: list[str]
    unchanged: list[str]
    errors: list[str]

    @property
    def ok(self) -> bool:
        return not self.errors


def sync(target: Path, entries: list[ResolvedEntry]) -> SyncResult:
    """Copie les entrées vers <target>/drydock/ (diff SHA-256) et écrit manifest.json.

    Le manifest n'est écrit, par replacement atomique, que si toutes les copies
    ont réussi. Le fichier destination s'appelle .tmp pendant l'écriture.
    """
    base = target / "drydock"
    copied: list[str] = []
    unchanged: list[str] = []
    errors: list[str] = []
    for e in entries:
        dest = base / e.rel_path
        try:
            if dest.is_file() and sha256_file(dest) == e.sha256:
                unchanged.append(e.rel_path)
            else:
                _atomic_write(dest, e.data)
                copied.append(e.rel_path)
        except OSError as exc:
            errors.append(f"{e.rel_path}: {exc}")
    try:
        stock = next(e for e in entries if e.rel_path == STOCK_REL)
        rescue = next(e for e in entries if e.rel_path == RESCUE_REL)
    except StopIteration:
        errors.append("entrées stock_v15 ou rescue_helper manquantes")
    if not errors:
        manifest = Manifest(
            generated_at=datetime.now(timezone.utc),
            stock_v15=stock.file_entry,
            rescue_helper=RescueHelper(file=rescue.rel_path, sha256=rescue.sha256),
            vessels=[e.file_entry for e in entries if e.rel_path.startswith("vessels/")],
        )
        _atomic_write(base / "manifest.json", manifest.to_json_bytes())
    return SyncResult(entries=entries, copied=copied, unchanged=unchanged, errors=errors)


@dataclass
class VerifyResult:
    ok: bool
    results: list[tuple[str, str]]  # (rel_path, "OK" | "MISSING" | "SHA MISMATCH")


def verify(target: Path) -> VerifyResult:
    """Vérifie chaque fichier listé dans manifest.json (existence + SHA-256)."""
    base = target / "drydock"
    man_file = base / "manifest.json"
    if not man_file.is_file():
        return VerifyResult(ok=False, results=[("manifest.json", "MISSING")])
    manifest = Manifest.model_validate_json(man_file.read_text())
    pairs = [(manifest.stock_v15.file, manifest.stock_v15.sha256)]
    pairs.append((manifest.rescue_helper.file, manifest.rescue_helper.sha256))
    pairs += [(v.file, v.sha256) for v in manifest.vessels]
    results: list[tuple[str, str]] = []
    for rel, sha in pairs:
        p = base / rel
        if not p.is_file():
            results.append((rel, "MISSING"))
        elif sha256_file(p) != sha:
            results.append((rel, "SHA MISMATCH"))
        else:
            results.append((rel, "OK"))
    return VerifyResult(ok=all(st == "OK" for _, st in results), results=results)
