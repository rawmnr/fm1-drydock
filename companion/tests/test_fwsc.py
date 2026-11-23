"""Tests hôte bit-à-bit du parseur C .fwsc contre la référence python (fm1_rescue.py).

Compile companion/proto/fwsc.c en exécutable natif, l'exécute sur de vrais .fwsc
téléchargés en Phase 1 (cache drydock-sync), et compare l'extraction de la zone
d'applications (produit, offset, sha256) à la référence python bit pour bit.
"""
from __future__ import annotations

import hashlib
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import pytest

PROTO = Path(__file__).resolve().parent.parent / "proto"


# ---- référence python, portage exact de fm1_rescue.py ----
def crc16(data: bytes, crc: int = 0) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
        crc &= 0xFFFF
    return crc


def enc(buf: bytearray, off: int, size: int, key: int = 0xFFFF) -> None:
    for i in range(off, off + size):
        buf[i] ^= key & 0xFF
        key = ((key << 1) ^ (0x1021 if key & 0x8000 else 0)) & 0xFFFF


def logical_image(raw: bytes) -> tuple[str, bytes]:
    product = "".join(
        chr((raw[i * 0x30 + 0x2F] - i - 1) & 0xFF)
        for i in range(20)
        if raw[i * 0x30 + 0x2F] != 0x7D
    )
    out = bytearray()
    for i in range(20):
        out += raw[i * 0x30 : i * 0x30 + 0x2F]
    out += raw[20 * 0x30 :]
    return product, bytes(out)


def app_area_reference(path: Path) -> tuple[str, bytes]:
    raw = path.read_bytes()
    product, img = logical_image(raw)
    hdr = bytearray(img[:0x40])
    enc(hdr, 0, 0x40)
    count = struct.unpack_from("<H", bytes(hdr), 8)[0]
    assert crc16(bytes(hdr[2:0x40])) == struct.unpack_from("<H", bytes(hdr), 0)[0]
    assert 1 <= count <= 11
    lst = img[0x40 : 0x40 + 0x50 * count]
    assert crc16(lst) == struct.unpack_from("<H", bytes(hdr), 2)[0]
    app = None
    for i in range(count):
        e = bytearray(lst[i * 0x50 : (i + 1) * 0x50])
        enc(e, 0, 0x50)
        typ, dcrc = struct.unpack_from("<H", bytes(e), 0)[0], struct.unpack_from("<H", bytes(e), 4)[0]
        off, size = struct.unpack_from("<II", bytes(e), 8)
        assert off + size <= len(img) and crc16(img[off : off + size]) == dcrc
        if typ == 0:
            app = img[off + 0x4000 : off + 0x93000]
    assert app is not None
    return product, app


# ---- compilation C hôte ----
def _cc() -> str | None:
    for c in ("cc", "gcc"):
        if shutil.which(c):
            return c
    # Windows : gcc mingw d'IDF (idf-git), si présent
    mingw = Path(r"C:/Espressif/tools/idf-git/2.44.0/mingw64/bin/gcc.exe")
    return str(mingw) if mingw.is_file() else None


@pytest.fixture(scope="module")
def extractor(tmp_path_factory) -> Path:
    cc = _cc()
    if cc is None:
        pytest.skip("aucun compilateur C hôte disponible")
    bin_path = tmp_path_factory.mktemp("bin") / ("fwsc_extract.exe" if sys.platform == "win32" else "fwsc_extract")
    subprocess.run([cc, "-O2", "-I", str(PROTO), str(PROTO / "fwsc.c"), str(PROTO / "main.c"), "-o", str(bin_path)], check=True)
    return bin_path


def _cache_fwsc() -> Path:
    p = Path.home() / ".cache" / "drydock-sync" / "vessels" / "sloop.fwsc"
    if p.is_file():
        return p
    pytest.skip("pas de .fwsc en cache (lancer d'abord: cd companion && uv run drydock fetch)")
    return p  # unreachable (pytest.skip lève)


def _run_extract(extractor: Path, fwsc: Path, app_out: Path) -> dict:
    res = subprocess.run([str(extractor), str(fwsc), str(app_out)], capture_output=True, text=True)
    assert res.returncode == 0, res.stderr
    return json.loads(res.stdout)


def test_c_framer_matches_python_reference(extractor: Path, tmp_path: Path) -> None:
    fwsc = _cache_fwsc()
    app_out = tmp_path / "sloop.app"
    out = _run_extract(extractor, fwsc, app_out)

    product, app = app_area_reference(fwsc)
    c_hash = hashlib.sha256(app_out.read_bytes()).hexdigest()
    py_hash = hashlib.sha256(app).hexdigest()

    assert out["product"] == product
    assert out["app_size"] == len(app) == 0x93000 - 0x4000
    assert c_hash == py_hash  # zone d'app bit à bit identique à fm1_rescue.py


def test_c_framer_rejects_official_v15(extractor: Path, tmp_path: Path) -> None:
    """Le V15 officiel n'est pas au format Felucca : le parseur C doit échouer proprement."""
    fwsc = Path.home() / ".cache" / "drydock-sync" / "stock" / "fm1_v15_official.fwsc"
    if not fwsc.is_file():
        pytest.skip("V15 officiel absent du cache")
    app_out = tmp_path / "v15.app"
    res = subprocess.run([str(extractor), str(fwsc), str(app_out)], capture_output=True, text=True)
    assert res.returncode != 0  # pas de marqueur FELUCCA-LOADER-1