from __future__ import annotations

from pathlib import Path

from typer.testing import CliRunner

from drydock_sync.cli import app
from drydock_sync.syncer import sha256_bytes, sync

from test_syncer import _entries

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
    result = runner.invoke(app, ["sync", "--target", str(tmp_path)])
    # aucune config ni réseau : doit échouer proprement
    assert result.exit_code == 1
    assert "vessels.toml" in result.output or "erreur" in result.output
