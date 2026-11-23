from __future__ import annotations

import json
from datetime import datetime

import pytest
from pydantic import ValidationError

from drydock_sync.models import (
    Manifest,
    RescueHelper,
    Source,
    VesselConfig,
    VesselsConfig,
    load_vessels,
)

SHA = "ab" * 32


def _manifest_dict(**over: object) -> dict:
    d = {
        "schema_version": 1,
        "generated_at": "2026-11-23T20:00:00Z",
        "stock_v15": {
            "id": "stock-v15",
            "name": "M-VAVE Official V15",
            "version": "15",
            "file": "stock/fm1_v15_official.fwsc",
            "size_bytes": 0,
            "sha256": SHA,
        },
        "rescue_helper": {
            "file": "rescue/jl_flash_helper.bin",
            "sha256": SHA,
        },
        "vessels": [
            {
                "id": "sloop",
                "name": "SLOOP",
                "version": "2.8.5",
                "source": "firmware_hub",
                "file": "vessels/sloop.fwsc",
                "size_bytes": 1234,
                "sha256": SHA,
            }
        ],
    }
    d.update(over)
    return d


def test_manifest_valid() -> None:
    m = Manifest.model_validate(_manifest_dict())
    assert m.schema_version == 1
    assert m.stock_v15.id == "stock-v15"
    assert m.vessels[0].source is Source.firmware_hub
    assert isinstance(m.generated_at, datetime)


def test_manifest_rejects_bad_sha256() -> None:
    with pytest.raises(ValidationError):
        Manifest.model_validate(_manifest_dict(stock_v15={**_manifest_dict()["stock_v15"], "sha256": "abc"}))


def test_manifest_rejects_extra_key() -> None:
    with pytest.raises(ValidationError):
        Manifest.model_validate(_manifest_dict(extra="x"))


def test_manifest_rejects_wrong_file_path() -> None:
    with pytest.raises(ValidationError):
        Manifest.model_validate(_manifest_dict(vessels=[{**_manifest_dict()["vessels"][0], "file": "other/sloop.fwsc"}]))


def test_rescue_helper_rejects_wrong_path() -> None:
    with pytest.raises(ValidationError):
        RescueHelper.model_validate({"file": "vessels/helper.bin", "sha256": SHA})


def test_manifest_rejects_empty_vessels() -> None:
    with pytest.raises(ValidationError):
        Manifest.model_validate(_manifest_dict(vessels=[]))


def test_manifest_roundtrip_json() -> None:
    m = Manifest.model_validate(_manifest_dict())
    data = json.loads(m.to_json_bytes())
    assert data["schema_version"] == 1
    assert data["vessels"][0]["sha256"] == SHA


def _config_toml() -> str:
    return """\
[[vessel]]
id = "sloop"
source = "firmware_hub"

[[vessel]]
id = "g1"
name = "GitHub one"
source = "github"
repo = "owner/repo"
asset = "x\\\\.fwsc"

[[vessel]]
id = "l1"
source = "local"
path = "backups/x.fwsc"
"""


def test_load_vessels(tmp_path) -> None:
    p = tmp_path / "vessels.toml"
    p.write_text(_config_toml(), encoding="utf-8")
    cfg = load_vessels(p)
    assert [v.id for v in cfg.vessel] == ["sloop", "g1", "l1"]
    assert cfg.vessel[1].source is Source.github


def test_github_requires_repo_and_asset() -> None:
    with pytest.raises(ValidationError):
        VesselConfig.model_validate({"id": "x", "source": "github"})


def test_local_requires_path() -> None:
    with pytest.raises(ValidationError):
        VesselConfig.model_validate({"id": "x", "source": "local"})


def test_vessel_id_charset_restricted() -> None:
    with pytest.raises(ValidationError):
        VesselsConfig.model_validate({"vessel": [{"id": "bad id", "source": "local", "path": "p"}]})


def test_empty_config_rejected() -> None:
    with pytest.raises(ValidationError):
        VesselsConfig.model_validate({"vessel": []})
