from __future__ import annotations

import hashlib

import httpx
import pytest

from drydock_sync.providers import firmware_hub as fh
from drydock_sync.providers.firmware_hub import FirmwareHub, check_sha256


def test_catalog_parsed(hub_client: httpx.Client, sample_catalog: dict) -> None:
    h = FirmwareHub(client=hub_client)
    cat = h.catalog()
    h.close()
    assert len(cat.firmwares) == 2
    assert cat.updated == sample_catalog["updated"]


def test_catalog_memoized(hub_client: httpx.Client) -> None:
    h = FirmwareHub(client=hub_client)
    assert h.catalog() is h.catalog()
    h.close()


def test_resolve_ok(hub_client: httpx.Client) -> None:
    h = FirmwareHub(client=hub_client)
    fw = h.resolve("sloop")
    h.close()
    assert fw.id == "sloop"
    assert fw.sha256 == hashlib.sha256(b"sloop").hexdigest()


def test_resolve_unknown_lists_known_ids(hub_client: httpx.Client) -> None:
    h = FirmwareHub(client=hub_client)
    with pytest.raises(KeyError) as exc:
        h.resolve("nope")
    h.close()
    assert "sloop" in str(exc.value)
    assert "felucca" in str(exc.value)


def test_download_pkg_ok(hub_client: httpx.Client) -> None:
    h = FirmwareHub(client=hub_client)
    fw = h.resolve("sloop")
    assert h.download_pkg(fw) == b"sloop"
    h.close()


def test_download_pkg_sha_mismatch_raises(sample_catalog: dict) -> None:
    # the served content does not match the catalog sha256
    def handler(request: httpx.Request) -> httpx.Response:
        if request.url.path == "/catalog.json":
            return httpx.Response(200, json=sample_catalog)
        return httpx.Response(200, content=b"tampered")

    client = httpx.Client(
        transport=httpx.MockTransport(handler), base_url="https://thefirmwarehub.com"
    )
    h = FirmwareHub(client=client)
    fw = h.resolve("sloop")
    with pytest.raises(ValueError, match="SHA-256"):
        h.download_pkg(fw)
    h.close()


def test_stock_v15_ok(hub_client: httpx.Client, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(fh, "V15_SHA256", hashlib.sha256(b"v15" * 500).hexdigest())
    h = FirmwareHub(client=hub_client)
    assert h.stock_v15() == b"v15" * 500
    h.close()


def test_stock_v15_sha_mismatch(hub_client: httpx.Client, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(fh, "V15_SHA256", "ff" * 32)
    h = FirmwareHub(client=hub_client)
    with pytest.raises(ValueError, match="SHA-256"):
        h.stock_v15()
    h.close()


def test_rescue_helper_ok(hub_client: httpx.Client, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(fh, "RESCUE_SHA256", hashlib.sha256(b"helper" * 100).hexdigest())
    h = FirmwareHub(client=hub_client)
    assert h.rescue_helper() == b"helper" * 100
    h.close()


def test_check_sha256_mismatch() -> None:
    with pytest.raises(ValueError, match="SHA-256"):
        check_sha256(b"data", "ff" * 32, "file.bin")
