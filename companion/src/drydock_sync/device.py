"""Détection du volume USB DRYDOCK et des périphériques USB d'intérêt (Windows/Linux/macOS)."""
from __future__ import annotations

import getpass
import platform
import re
import shutil
import subprocess
from pathlib import Path

VOLUME_NAME = "DRYDOCK"
ESP32 = ("303A", "1001")  # Waveshare ESP32-S3-Touch-AMOLED-1.8, USB natif
FM1 = ("4D4A", "4155")  # M-VAVE FM-1, USB-MIDI


def find_drydock_volume() -> Path | None:
    """Rend le chemin du volume monté nommé DRYDOCK, ou None."""
    system = platform.system()
    if system == "Windows":
        return _windows_volume()
    if system == "Darwin":
        p = Path("/Volumes") / VOLUME_NAME
        return p if p.is_dir() else None
    for base in (Path("/media"), Path("/mnt"), Path("/run/media") / getpass.getuser()):
        p = base / VOLUME_NAME
        if p.is_dir():
            return p
    return None


def _windows_volume() -> Path | None:
    ps = (
        "Get-CimInstance Win32_LogicalDisk | Where-Object { $_.VolumeName -eq 'DRYDOCK' }"
        " | Select-Object -ExpandProperty DeviceID"
    )
    try:
        out = subprocess.run(
            ["powershell", "-NoProfile", "-Command", ps],
            capture_output=True, text=True, timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    m = re.search(r"^([A-Za-z]:)\s*$", out.stdout or "", re.M)
    return Path(m.group(1) + "\\") if m else None


def usb_devices() -> list[tuple[str, str]]:
    """Rend les (VID, PID) en majuscules de chaque périphérique USB connecté."""
    system = platform.system()
    if system == "Windows":
        return _windows_usb()
    if system == "Darwin":
        return _darwin_usb()
    return _linux_usb()


def _windows_usb() -> list[tuple[str, str]]:
    ps = (
        "Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'VID_' }"
        " | Select-Object -ExpandProperty InstanceId"
    )
    try:
        out = subprocess.run(
            ["powershell", "-NoProfile", "-Command", ps],
            capture_output=True, text=True, timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired):
        return []
    pairs = re.findall(r"VID_([0-9A-Fa-f]{4})&PID_([0-9A-Fa-f]{4})", out.stdout or "")
    return sorted({(v.upper(), p.upper()) for v, p in pairs})


def _linux_usb() -> list[tuple[str, str]]:
    if shutil.which("lsusb"):
        try:
            out = subprocess.run(["lsusb"], capture_output=True, text=True, timeout=30).stdout
        except OSError:
            return []
        return sorted(set(re.findall(r"ID ([0-9a-fA-F]{4}):([0-9a-fA-F]{4})", out)))
    sysfs = Path("/sys/bus/usb/devices")
    if not sysfs.is_dir():
        return []
    pairs = []
    for dev in sysfs.iterdir():
        try:
            vid = (dev / "idVendor").read_text().strip()
            pid = (dev / "idProduct").read_text().strip()
        except OSError:
            continue
        if vid and pid:
            pairs.append((vid.upper(), pid.upper()))
    return sorted(set(pairs))


def _darwin_usb() -> list[tuple[str, str]]:
    try:
        out = subprocess.run(
            ["system_profiler", "SPUSBDataType"], capture_output=True, text=True, timeout=60
        ).stdout
    except OSError:
        return []
    pairs: list[tuple[str, str]] = []
    product: str | None = None
    for line in out.splitlines():
        m_prod = re.search(r"Product ID:\s*0x([0-9a-fA-F]{4})", line)
        m_vend = re.search(r"Vendor ID:\s*0x([0-9a-fA-F]{4})", line)
        if m_prod:
            product = m_prod.group(1)
        elif m_vend and product:
            pairs.append((m_vend.group(1).upper(), product.upper()))
            product = None
    return sorted(set(pairs))
