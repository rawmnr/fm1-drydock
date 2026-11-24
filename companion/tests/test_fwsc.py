"""Host bit-for-bit tests of the C protocol (drydock_proto) against the Python
reference: .fwsc parser (fm1_rescue.py), JieLi ciphers (jielib.c) and
M-UPGRADE framing from the hub (upgrade.c).

Compiles firmware/components/drydock_proto into a native executable, runs it on
real .fwsc (drydock-sync cache or downloaded) and compares bit for bit with the
Python references. Skipped if no host C compiler is available.
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

PROTO = Path(__file__).resolve().parent.parent.parent / "firmware" / "components" / "drydock_proto"


# ---- Python reference, exact port of fm1_rescue.py ----
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


def sfc(buf: bytearray, size: int, key: int) -> None:
    for i in range(0, size, 32):
        enc(buf, i, min(size - i, 32), key ^ (i >> 2))


MAGIC = "孟黎我爱你，玉林".encode("gb2312")


def mengli(data: bytes) -> bytes:
    out = bytearray(data)
    crc = crc16(b"\xff\xff", 0xFFFF)
    for i in range(len(out)):
        crc = crc16(MAGIC[i % len(MAGIC) : i % len(MAGIC) + 1], crc)
        out[i] ^= crc & 0xFF
    return bytes(out)


def chip_key(blob: bytes) -> int | None:
    assert len(blob) == 34
    if crc16(blob[:32]) != struct.unpack_from("<H", blob, 32)[0]:
        return None
    s = sum(blob[:16]) & 0xFF
    s = 0xAA if s >= 0xE0 else 0x55 if s <= 0x10 else s
    return sum(1 << i for i in range(16) if (blob[16 + i] ^ blob[15 - i]) < s)


def cdb(op: int, sub: int, args: bytes) -> bytes:
    c = bytes([op, sub]) + args
    return c + b"\xFF" * (16 - len(c))


# ---- Python reference, exact port of the hub (upgrade.c) ----
def pack7(data: bytes) -> bytes:
    out = bytearray()
    acc = 0
    nb = 0
    for b in data:
        acc |= b << nb
        nb += 8
        while nb >= 7:
            out.append(acc & 0x7F)
            acc >>= 7
            nb -= 7
    if nb:
        out.append(acc & 0x7F)
    return bytes(out)


def unpack7(s: bytes) -> bytes:
    out = bytearray()
    acc = 0
    nb = 0
    for b in s:
        acc |= (b & 0x7F) << nb
        nb += 7
        while nb >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nb -= 8
    return bytes(out)


def upgrade_response(addr: int, data: bytes, fl: int = 0) -> bytes:
    body = bytearray(
        [0x00, 0x59, 0x30, (len(data) + 8) & 0xFF, ((len(data) + 8) >> 8) & 0xFF, 0, fl]
    )
    body += struct.pack("<I", addr)
    body += struct.pack("<H", len(data))
    body += b"\x00" + data
    s = sum(body[6:]) & 0xFF
    body.append(~s & 0xFF)
    return bytes([0xF0]) + pack7(bytes(body)) + bytes([0xF7])


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


# ---- host C compilation ----
def _cc() -> str | None:
    for c in ("cc", "gcc"):
        if shutil.which(c):
            return c
    # Windows: gcc mingw from IDF (idf-git), if present
    mingw = Path(r"C:/Espressif/tools/idf-git/2.44.0/mingw64/bin/gcc.exe")
    if mingw.is_file():
        return str(mingw)
    # Windows: zig (host cc) installed via pip (ziglang) in the venv
    import ziglang

    z = Path(ziglang.__file__).parent / "zig.exe"
    if z.is_file():
        return str(z)
    return None


def _cc_cmd(cc: str) -> list[str]:
    """Zig serves as a C compiler via the `cc` subcommand (gcc calls it directly)."""
    if Path(cc).name.lower().startswith("zig"):
        return [cc, "cc"]
    return [cc]


@pytest.fixture(scope="module")
def extractor(tmp_path_factory) -> Path:
    cc = _cc()
    if cc is None:
        pytest.skip("no host C compiler available")
    bin_dir = tmp_path_factory.mktemp("bin")
    exe = bin_dir / ("fwsc_extract.exe" if sys.platform == "win32" else "fwsc_extract")
    subprocess.run(
        [*_cc_cmd(cc), "-O2", "-I", str(PROTO), str(PROTO / "jielib.c"), str(PROTO / "fwsc.c"),
         str(PROTO / "main.c"), "-o", str(exe)],
        check=True,
    )
    return exe


@pytest.fixture(scope="module")
def harness(tmp_path_factory) -> Path:
    cc = _cc()
    if cc is None:
        pytest.skip("no host C compiler available")
    bin_dir = tmp_path_factory.mktemp("bin")
    exe = bin_dir / ("proto_harness.exe" if sys.platform == "win32" else "proto_harness")
    subprocess.run(
        [*_cc_cmd(cc), "-O2", "-I", str(PROTO), str(PROTO / "jielib.c"), str(PROTO / "upgrade.c"),
         str(PROTO / "fwsc.c"), str(PROTO / "harness.c"), "-o", str(exe)],
        check=True,
    )
    return exe


def _run_harness(harness: Path, mode: str, data: bytes, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(harness), mode, *args], input=data, capture_output=True,
    )


def _fetch_hub(tmp_path: Path, id_: str, name: str) -> Path:
    """Download a real .fwsc from the hub if absent from the local cache (allows running in CI)."""
    from drydock_sync.providers.firmware_hub import FirmwareHub

    cached = Path.home() / ".cache" / "drydock-sync" / "vessels" / f"{id_}.fwsc"
    if cached.is_file():
        return cached
    with FirmwareHub() as hub:
        fw = hub.resolve(id_)
        data = hub.download_pkg(fw)
    p = tmp_path / f"{name}.fwsc"
    p.write_bytes(data)
    return p


def _run_extract(extractor: Path, fwsc: Path, app_out: Path) -> dict:
    res = subprocess.run([str(extractor), str(fwsc), str(app_out)], capture_output=True, text=True)
    assert res.returncode == 0, res.stderr
    return json.loads(res.stdout)


def test_c_framer_matches_python_reference(extractor: Path, tmp_path: Path) -> None:
    fwsc = _fetch_hub(tmp_path, "sloop", "sloop")
    app_out = tmp_path / "sloop.app"
    out = _run_extract(extractor, fwsc, app_out)

    product, app = app_area_reference(fwsc)
    c_hash = hashlib.sha256(app_out.read_bytes()).hexdigest()
    py_hash = hashlib.sha256(app).hexdigest()

    assert out["product"] == product
    assert out["app_size"] == len(app) == 0x93000 - 0x4000
    assert c_hash == py_hash  # app area bit for bit identical to fm1_rescue.py


def test_c_framer_rejects_official_v15(extractor: Path, tmp_path: Path) -> None:
    """The official V15 is not in Felucca format: the C parser must fail cleanly."""
    from drydock_sync.providers.firmware_hub import FirmwareHub

    cached = Path.home() / ".cache" / "drydock-sync" / "stock" / "fm1_v15_official.fwsc"
    if cached.is_file():
        fwsc = cached
    else:
        with FirmwareHub() as hub:
            data = hub.stock_v15()
        fwsc = tmp_path / "v15.fwsc"
        fwsc.write_bytes(data)
    app_out = tmp_path / "v15.app"
    res = subprocess.run([str(extractor), str(fwsc), str(app_out)], capture_output=True, text=True)
    assert res.returncode != 0  # no FELUCCA-LOADER-1 marker


def test_pack7_unpack7_roundtrip(harness: Path) -> None:
    data = bytes(range(256)) * 3
    packed = _run_harness(harness, "pack7", data).stdout
    assert packed == pack7(data)  # bit for bit vs the hub
    back = _run_harness(harness, "unpack7", packed).stdout
    assert back == data


def test_upgrade_response_matches_hub(harness: Path) -> None:
    for addr, fl, data in (
        (0x00000100, 0, bytes(range(64))),
        (0x00001000, 3, bytes(range(256)) * 2),  # max size 512
        (0xE0000000, 0, b"success\x00"),     # end response
        (0xF0000000, 0, b"success\x00"),
    ):
        res = _run_harness(harness, "response", data, f"{addr}", f"{fl}", f"{len(data)}")
        assert res.returncode == 0
        assert res.stdout == upgrade_response(addr, data, fl)  # bit for bit vs the hub


def test_upgrade_request_parse(harness: Path) -> None:
    # built by the Python reference (same framing as the hub)
    for addr, ln, fl in ((0x00000000, 64, 0), (0x00001000, 512, 5)):
        body = bytearray([0x00, 0x59, 0x30, 0, 0, 0, fl])
        body += struct.pack("<I", addr) + struct.pack("<H", ln) + b"\x00"
        s = sum(body[6:]) & 0xFF
        body.append(~s & 0xFF)
        pkt = bytes([0xF0]) + pack7(bytes(body)) + bytes([0xF7])
        res = _run_harness(harness, "req-parse", pkt)
        assert res.returncode == 0
        assert res.stdout == f"OK addr=0x{addr:08X} len={ln} fl={fl}\n".encode()


def test_upgrade_identity_parse(harness: Path) -> None:
    # FM-1_900 and ota-FM-1_900 (loader) identities
    for text in (b"FM-1_900", b"ota-FM-1_900", b"SLOOP_26"):
        body = bytearray([0x00, 0x59, 0x11]) + b"\x00" * 3 + text + b"\x00" * (34 - 6 - len(text))
        pkt = bytes([0xF0]) + pack7(bytes(body)) + bytes([0xF7])
        res = _run_harness(harness, "id-parse", pkt)
        assert res.returncode == 0
        assert res.stdout.strip() == text


def test_mengli_matches_python(harness: Path) -> None:
    data = bytes(range(256))
    assert _run_harness(harness, "mengli", data).stdout == mengli(data)
    # self-inverse (cipher property)
    ciphered = _run_harness(harness, "mengli", mengli(data)).stdout
    assert ciphered == data


def test_cdb_matches_python(harness: Path) -> None:
    for op, sub, args in (
        (0xFB, 0x06, b"\x01\xC0\x20\x00\x02\x00\x00\x12\x34"),  # mem_write
        (0xFD, 0x07, b"\x01\xC0\x20\x00\x00\x10"),              # mem_read
        (0xFC, 0x0B, b""),                                      # read_id
        (0xFB, 0x01, b"\x00\x40\x00\x00"),                      # erase sector
    ):
        res = _run_harness(harness, "cdb", args, f"{op}", f"{sub}", f"{len(args)}")
        assert res.returncode == 0
        assert res.stdout == cdb(op, sub, args)


def test_chip_key_matches_python(harness: Path) -> None:
    import os

    blob = bytearray(os.urandom(32))
    blob += struct.pack("<H", crc16(bytes(blob)))
    key = chip_key(bytes(blob))
    res = _run_harness(harness, "chipkey", bytes(blob))
    assert res.returncode == 0
    assert res.stdout == f"KEY=0x{key:04X}\n".encode()
    # invalid CRC -> rejected
    blob[0] ^= 0xFF
    res = _run_harness(harness, "chipkey", bytes(blob))
    assert res.returncode != 0


def test_product_of_matches_python(harness: Path, tmp_path: Path) -> None:
    fwsc = _fetch_hub(tmp_path, "skiff", "skiff")
    raw = fwsc.read_bytes()
    product, _ = logical_image(raw)
    res = _run_harness(harness, "product", raw)
    assert res.returncode == 0
    assert res.stdout.strip().decode() == product
