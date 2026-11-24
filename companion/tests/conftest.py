from __future__ import annotations

import hashlib

import httpx
import pytest


def _firmware(id_: str, name: str, version: str, kind, what: str) -> dict:
    payload = id_.encode()  # the package served by the mock is the encoded id
    return {
        "id": id_,
        "name": name,
        "version": version,
        "author": "author",
        "pkg": f"fw/{id_}-{version}.fwsc",
        "size": 610286,
        "sha256": hashlib.sha256(payload).hexdigest(),
        "what": what,
        "kind": kind,
        "chip": "new",
    }


SAMPLE_CATALOG = {
    "license": {"license": "CC-BY-4.0"},
    "updated": "2026-11-23 18:02 UTC",
    "firmwares": [
        _firmware("sloop", "SLOOP", "2.8.5", "groovebox", "Live groovebox for FM-1"),
        _firmware("felucca", "Felucca", "1.8.3", ["groove", "dev"], "The firmware the others build on"),
    ],
}


@pytest.fixture
def sample_catalog() -> dict:
    return SAMPLE_CATALOG


@pytest.fixture
def hub_client(sample_catalog: dict) -> httpx.Client:
    """Mocked hub client: consistent catalog and packages (content = encoded id)."""
    payloads = {f"/{f['pkg']}": f["id"].encode() for f in sample_catalog["firmwares"]}

    def handler(request: httpx.Request) -> httpx.Response:
        path = request.url.path
        if path == "/catalog.json":
            return httpx.Response(200, json=sample_catalog)
        if path in payloads:
            return httpx.Response(200, content=payloads[path])
        if path == "/fw/mvave/FM-1_015.fwsc":
            return httpx.Response(200, content=b"v15" * 500)
        if "wl82loader.bin" in path:
            return httpx.Response(200, content=b"helper" * 100)
        return httpx.Response(404)

    return httpx.Client(
        transport=httpx.MockTransport(handler), base_url="https://thefirmwarehub.com"
    )
