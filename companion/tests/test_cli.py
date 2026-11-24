from __future__ import annotations

from pathlib import Path

from test_syncer import _entries
from typer.testing import CliRunner

from drydock_sync.cli import app
from drydock_sync.syncer import sync

runner = CliRunner()


def test_help_lists_commands() -> None:
    result = runner.invoke(app, ["--help"])
    assert result.exit_code == 0
    for name in ("probe", "fetch", "list", "sync", "verify"):
        assert name in result.output
    assert "list_vessels" not in result.output


def test_verify_ok_via_cli(tmp_path: Path) -> None:
    sync(tmp_path, _entries())
    result = runner.invoke(app, ["verify", "--target", str(tmp_path)])
    assert result.exit_code == 0
    assert "OK" in result.output


def test_verify_missing_target_fails(tmp_path: Path) -> None:
    result = runner.invoke(app, ["verify", "--target", str(tmp_path / "nope")])
    assert result.exit_code == 1
    assert "MISSING" in result.output


def test_sync_writes_manifest_via_cli(tmp_path: Path) -> None:
    # config explicitly absent: fails cleanly (independent of the CWD,
    # where a real user vessels.toml may exist)
    result = runner.invoke(
        app, ["sync", "--target", str(tmp_path), "--config", str(tmp_path / "nope.toml")]
    )
    assert result.exit_code == 1
    assert "vessels.toml" in result.output or "error" in result.output


def test_doctor_reports_all_checks(tmp_path: Path, monkeypatch, sample_catalog: dict) -> None:
    from drydock_sync import cli as cli_mod
    from drydock_sync import device as device_mod
    from drydock_sync.providers.firmware_hub import FirmwareHub

    cfg = tmp_path / "vessels.toml"
    cfg.write_text("[[vessel]]\nid = \"sloop\"\nsource = \"firmware_hub\"\n", encoding="utf-8")

    # responsive fake hub
    from drydock_sync.providers.firmware_hub import HubCatalog

    class FakeHub(FirmwareHub):
        def catalog(self):
            return HubCatalog.model_validate(sample_catalog)

        def close(self):
            pass

    monkeypatch.setattr(cli_mod, "FirmwareHub", FakeHub)
    monkeypatch.setattr(device_mod, "usb_devices", lambda: [("303A", "1001"), ("4D4A", "4155")])
    monkeypatch.setattr(device_mod, "find_drydock_volume", lambda: None)

    result = runner.invoke(app, ["doctor", "--config", str(cfg), "--cache", str(tmp_path / "cache")])
    assert result.exit_code == 1  # volume missing -> failure
    for name in ("config", "cache", "hub", "usb:esp32", "usb:fm1", "volume"):
        assert name in result.output
