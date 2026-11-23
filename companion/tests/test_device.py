from __future__ import annotations

import subprocess

from drydock_sync import device


def _run_side_effect(stdout: str):
    return lambda *args, **kwargs: subprocess.CompletedProcess(
        args=[], returncode=0, stdout=stdout
    )


def test_windows_volume_detected(monkeypatch) -> None:
    monkeypatch.setattr(device.platform, "system", lambda: "Windows")
    monkeypatch.setattr(device.subprocess, "run", _run_side_effect("E:\r\n"))
    assert device.find_drydock_volume() is not None
    assert str(device.find_drydock_volume()) == "E:\\"


def test_windows_volume_absent(monkeypatch) -> None:
    monkeypatch.setattr(device.platform, "system", lambda: "Windows")
    monkeypatch.setattr(device.subprocess, "run", _run_side_effect("  \r\n"))
    assert device.find_drydock_volume() is None


def test_windows_usb_pnp_parsed(monkeypatch) -> None:
    out = (
        "USB\\VID_303A&PID_1001&MI_00\\6&1&\n"
        "USB\\VID_4D4A&PID_4155&MI_00\\6&2&\n"
        "USB\\VID_27C6&PID_5395\\HTK32\n"
    )
    monkeypatch.setattr(device.platform, "system", lambda: "Windows")
    monkeypatch.setattr(device.subprocess, "run", _run_side_effect(out))
    assert device.usb_devices() == [
        ("27C6", "5395"),
        ("303A", "1001"),
        ("4D4A", "4155"),
    ]


def test_linux_volume_detection(monkeypatch) -> None:
    monkeypatch.setattr(device.platform, "system", lambda: "Linux")
    monkeypatch.setattr(device.getpass, "getuser", lambda: "u")
    monkeypatch.setattr(device.Path, "is_dir", lambda self: self.name == "DRYDOCK")
    assert device.find_drydock_volume() == device.Path("/media/DRYDOCK")
