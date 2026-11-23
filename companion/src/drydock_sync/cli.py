"""Interface CLI drydock : probe, fetch, list, sync, verify."""
from __future__ import annotations

import os
from pathlib import Path
from typing import Optional

import typer

from . import device
from .models import Source, load_vessels
from .providers.firmware_hub import FirmwareHub
from .providers.github import GithubProvider
from .syncer import Cache, Resolver, resolve_all, sync as run_sync, verify as run_verify

app = typer.Typer(
    help="drydock-sync: fetch FM-1 firmwares and sync the DRYDOCK USB-C volume.",
    add_completion=False,
)


def _cache_root(explicit: Optional[Path]) -> Path:
    env = os.environ.get("DRYDOCK_CACHE")
    if explicit is not None:
        return explicit
    if env:
        return Path(env)
    return Path.home() / ".cache" / "drydock-sync"


def _print_entry(e) -> None:
    print(f"  {e.id:20} {e.name[:28]:28} {e.version or '-':8} {len(e.data):>8} B  sha256 {e.sha256[:16]}")


def _fail(message: str) -> None:
    typer.secho(f"erreur: {message}", fg=typer.colors.RED, err=True)
    raise typer.Exit(1)


@app.command()
def probe() -> None:
    """Inspect the remote catalog and the USB devices connected to the PC."""
    with FirmwareHub() as hub:
        try:
            cat = hub.catalog()
        except Exception as exc:  # réseau, JSON invalide
            _fail(f"catalogue thefirmwarehub.com inaccessible: {exc}")
    print(f"catalogue: {len(cat.firmwares)} firmwares, mise à jour {cat.updated}")
    vol = device.find_drydock_volume()
    print(f"volume DRYDOCK: {vol if vol else 'introuvable'}")
    for vid, pid in device.usb_devices():
        if (vid, pid) == device.ESP32:
            name = "ESP32-S3 (carte Drydock)"
        elif (vid, pid) == device.FM1:
            name = "FM-1 (USB-MIDI)"
        else:
            name = ""
        print(f"  USB {vid}:{pid} {name}".rstrip())


@app.command("list")
def list_vessels(
    config: Path = typer.Option("vessels.toml", "--config", "-c", help="vessels.toml to resolve"),
) -> None:
    """Resolve the configured vessels to their latest versions, without downloading."""
    try:
        cfg = load_vessels(config)
    except Exception as exc:
        _fail(str(exc))
    with FirmwareHub() as hub:
        for v in cfg.vessel:
            if v.source is Source.firmware_hub:
                try:
                    fw = hub.resolve(v.id)
                    print(f"  {v.id:20} hub      {fw.version:8} {fw.pkg}  {fw.sha256[:16]} ({fw.size or '?'} B)")
                except Exception as exc:
                    _fail(str(exc))
            elif v.source is Source.github:
                github = GithubProvider()
                try:
                    release = github.latest_release(v.repo)
                    asset = github.find_asset(release, v.asset)
                except Exception as exc:
                    github.close()
                    _fail(str(exc))
                github.close()
                print(f"  {v.id:20} github   {release.get('tag_name', '?'):8} {asset['name']}  ({asset.get('size', '?')} B)")
            else:
                p = (config.parent / v.path).resolve()
                try:
                    size = p.stat().st_size
                except OSError:
                    _fail(f"{p}: fichier local absent")
                print(f"  {v.id:20} local    {size} B  {p}")


@app.command()
def fetch(
    config: Path = typer.Option("vessels.toml", "--config", "-c", help="vessels.toml"),
    cache: Optional[Path] = typer.Option(None, "--cache", help="cache dir (default ~/.cache/drydock-sync)"),
) -> None:
    """Download configured vessels, official Stock V15 and the rescue helper into the cache."""
    try:
        cfg = load_vessels(config)
    except Exception as exc:
        _fail(str(exc))
    with FirmwareHub() as hub:
        resolver = Resolver(hub, GithubProvider(), Cache(_cache_root(cache)), config.parent)
        try:
            entries = resolve_all(resolver, cfg)
        except Exception as exc:
            _fail(str(exc))
    print(f"cache: {resolver.cache.root}")
    for e in entries:
        _print_entry(e)
    print(f"{len(entries)} fichiers prêts à synchroniser")


@app.command()
def sync(
    target: Optional[Path] = typer.Option(None, "--target", "-t", help="path to the DRYDOCK volume (default: auto-detect)"),
    config: Path = typer.Option("vessels.toml", "--config", "-c", help="vessels.toml"),
    cache: Optional[Path] = typer.Option(None, "--cache", help="cache dir (default ~/.cache/drydock-sync)"),
) -> None:
    """Copy changed firmwares to <target>/drydock/ and write manifest.json atomically."""
    try:
        cfg = load_vessels(config)
    except Exception as exc:
        _fail(str(exc))
    dest = target if target is not None else device.find_drydock_volume()
    if dest is None:
        _fail("volume DRYDOCK introuvable: branch the card in PC Sync mode, or pass --target PATH")
    with FirmwareHub() as hub:
        resolver = Resolver(hub, GithubProvider(), Cache(_cache_root(cache)), config.parent)
        try:
            entries = resolve_all(resolver, cfg)
            result = run_sync(dest, entries)
        except Exception as exc:
            _fail(str(exc))
    for rel in result.copied:
        print(f"écrit  {rel}")
    for rel in result.unchanged:
        print(f"ok     {rel}")
    for err in result.errors:
        typer.secho(f"erreur {err}", fg=typer.colors.RED, err=True)
    if not result.errors:
        print(f"manifest.json écrit dans {dest / 'drydock'}")
    else:
        raise typer.Exit(1)


@app.command()
def verify(
    target: Optional[Path] = typer.Option(None, "--target", "-t", help="path to the DRYDOCK volume (default: auto-detect)"),
) -> None:
    """Check every file listed in manifest.json (existence and SHA-256)."""
    dest = target if target is not None else device.find_drydock_volume()
    if dest is None:
        _fail("volume DRYDOCK introuvable: branch the card in PC Sync mode, or pass --target PATH")
    try:
        result = run_verify(dest)
    except Exception as exc:
        _fail(str(exc))
    for rel, status in result.results:
        color = typer.colors.GREEN if status == "OK" else typer.colors.RED
        typer.secho(f"{status:12} {rel}", fg=color)
    if not result.ok:
        raise typer.Exit(1)
    print("vérification OK")


if __name__ == "__main__":
    app()
