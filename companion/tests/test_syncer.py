from __future__ import annotations

import json
from pathlib import Path

import jsonschema
import pytest

from drydock_sync.models import Manifest, Source
from drydock_sync.syncer import Cache, ResolvedEntry, sha256_bytes, sync, verify

SCHEMA = Path(__file__).resolve().parents[2] / "schema" / "manifest.schema.json"

V15_DATA = b"v15" * 400
HELPER_DATA = b"helper" * 300
SLOOP_DATA = b"sloop" * 200


def _entries() -> list[ResolvedEntry]:
    return [
        ResolvedEntry(
            id="stock-v15", name="M-VAVE Official V15", rel_path="stock/fm1_v15_official.fwsc",
            data=V15_DATA, sha256=sha256_bytes(V15_DATA), version="15",
        ),
        ResolvedEntry(
            id="rescue-helper", name="JieLi flash helper (UBOOT1.00)",
            rel_path="rescue/jl_flash_helper.bin", data=HELPER_DATA, sha256=sha256_bytes(HELPER_DATA),
        ),
        ResolvedEntry(
            id="sloop", name="SLOOP", rel_path="vessels/sloop.fwsc", data=SLOOP_DATA,
            sha256=sha256_bytes(SLOOP_DATA), version="2.8.5", category="groovebox",
            description="Live groovebox for FM-1", source=Source.firmware_hub,
        ),
    ]


def _validate_against_schema(manifest_path: Path) -> None:
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    jsonschema.validate(manifest, schema)


def test_sync_creates_layout_and_valid_manifest(tmp_path: Path) -> None:
    result = sync(tmp_path, _entries())
    assert result.ok
    base = tmp_path  # racine du volume == racine de la layout Drydock
    assert (base / "stock/fm1_v15_official.fwsc").read_bytes() == V15_DATA
    assert (base / "rescue/jl_flash_helper.bin").read_bytes() == HELPER_DATA
    assert (base / "vessels/sloop.fwsc").read_bytes() == SLOOP_DATA
    manifest = Manifest.model_validate_json((base / "manifest.json").read_text())
    assert manifest.schema_version == 1
    assert manifest.stock_v15.file == "stock/fm1_v15_official.fwsc"
    assert manifest.vessels[0].sha256 == sha256_bytes(SLOOP_DATA)
    _validate_against_schema(base / "manifest.json")


def test_sync_is_idempotent(tmp_path: Path) -> None:
    first = sync(tmp_path, _entries())
    second = sync(tmp_path, _entries())
    assert first.copied == [e.rel_path for e in _entries()]
    assert second.copied == []
    assert sorted(second.unchanged) == sorted(e.rel_path for e in _entries())


def test_no_tmp_file_left(tmp_path: Path) -> None:
    sync(tmp_path, _entries())
    leftover = list(tmp_path.rglob("*.tmp"))
    assert leftover == []


def test_verify_ok_after_sync(tmp_path: Path) -> None:
    sync(tmp_path, _entries())
    result = verify(tmp_path)
    assert result.ok
    assert all(st == "OK" for _, st in result.results)


def test_verify_detects_tamper(tmp_path: Path) -> None:
    sync(tmp_path, _entries())
    (tmp_path / "vessels/sloop.fwsc").write_bytes(b"evil")
    result = verify(tmp_path)
    assert not result.ok
    assert ("vessels/sloop.fwsc", "SHA MISMATCH") in result.results


def test_verify_missing_manifest(tmp_path: Path) -> None:
    result = verify(tmp_path)
    assert not result.ok
    assert result.results == [("manifest.json", "MISSING")]


def test_sync_requires_stock_and_rescue(tmp_path: Path) -> None:
    only_vessel = [e for e in _entries() if e.rel_path.startswith("vessels/")]
    result = sync(tmp_path, only_vessel)
    assert not result.ok
    assert not (tmp_path / "manifest.json").exists()


def test_cache_ensure_writes_once(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    cache = Cache(tmp_path / "cache")
    calls: list[int] = []

    def producer() -> bytes:
        calls.append(1)
        return b"data"

    data = cache.ensure("vessels/x.fwsc", sha256_bytes(b"data"), producer)
    assert data == b"data"
    data2 = cache.ensure("vessels/x.fwsc", sha256_bytes(b"data"), producer)
    assert data2 == b"data"
    assert len(calls) == 1
    assert cache.valid("vessels/x.fwsc", sha256_bytes(b"data"))


def test_cache_redownloads_on_sha_change(tmp_path: Path) -> None:
    cache = Cache(tmp_path / "cache")
    cache.ensure("vessels/x.fwsc", sha256_bytes(b"one"), lambda: b"one")
    with pytest.raises(ValueError, match="SHA-256"):
        cache.ensure("vessels/x.fwsc", sha256_bytes(b"two"), lambda: b"wrong")
