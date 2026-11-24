"""Client for the thefirmwarehub.com catalog.

Sources measured on 2026-11-23:
- catalog: GET /catalog.json, CC-BY-4.0 license, read at most once a day.
- Stock V15: hub mirror of sdk/firmware/FM-1_015.fwsc (m-vave/fm1_open, Apache-2.0).
- rescue helper: wl82loader.bin from jl-uboot-tool (MIT, kagaimiq), pinned commit
  (same constants as fm1_rescue.py).
"""
from __future__ import annotations

import hashlib

import httpx
from pydantic import BaseModel, ConfigDict

from ..models import Sha256

HUB = "https://thefirmwarehub.com"
V15_REL = "fw/mvave/FM-1_015.fwsc"
V15_SHA256 = "db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a"

JL_COMMIT = "adb3f18889e88ac512ce0a3c4d8cc3d3cb30696a"
JL_LOADER_PATH = "data/loaderblobs/usb/wl82loader.bin"
RESCUE_URL = f"https://raw.githubusercontent.com/kagaimiq/jl-uboot-tool/{JL_COMMIT}/{JL_LOADER_PATH}"
RESCUE_SHA256 = "d41da6126760c9d66660bcc0cac8d27d221806c5e369a8036921efe68dca5376"


def check_sha256(data: bytes, expected: str, name: str) -> None:
    got = hashlib.sha256(data).hexdigest()
    if got != expected:
        raise ValueError(f"{name}: SHA-256 {got} != expected {expected}")


class HubFirmware(BaseModel):
    """A catalog entry. Unused fields are ignored."""

    model_config = ConfigDict(extra="ignore")

    id: str
    name: str
    version: str
    pkg: str
    sha256: Sha256
    size: int | None = None
    author: str | None = None
    repo: str | None = None
    what: str | None = None
    kind: str | list[str] | None = None
    product: str | None = None
    chip: str | None = None
    lic: str | None = None
    beta: bool = False


class HubCatalog(BaseModel):
    """GET /catalog.json : {license, updated, firmwares: [...]}."""

    model_config = ConfigDict(extra="ignore")

    license: dict[str, object] | None = None
    updated: str | None = None
    firmwares: list[HubFirmware]


class FirmwareHub:
    """HTTP client for the hub. One catalog per instance (memoized)."""

    def __init__(self, client: httpx.Client | None = None):
        self._client = client or httpx.Client(base_url=HUB, timeout=60, follow_redirects=True)
        self._catalog: HubCatalog | None = None

    def __enter__(self) -> FirmwareHub:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def close(self) -> None:
        self._client.close()

    def catalog(self) -> HubCatalog:
        if self._catalog is None:
            r = self._client.get("/catalog.json")
            r.raise_for_status()
            self._catalog = HubCatalog.model_validate(r.json())
        return self._catalog

    def resolve(self, fw_id: str, catalog: HubCatalog | None = None) -> HubFirmware:
        cat = catalog or self.catalog()
        for fw in cat.firmwares:
            if fw.id == fw_id:
                return fw
        ids = ", ".join(sorted(fw.id for fw in cat.firmwares))
        raise KeyError(f"id '{fw_id}' missing from catalog. Known ids: {ids}")

    def download_pkg(self, fw: HubFirmware) -> bytes:
        url = fw.pkg if fw.pkg.startswith("http") else f"{HUB}/{fw.pkg}"
        r = self._client.get(url)
        r.raise_for_status()
        data = r.content
        check_sha256(data, fw.sha256, fw.pkg)
        return data

    def stock_v15(self) -> bytes:
        r = self._client.get(f"{HUB}/{V15_REL}")
        r.raise_for_status()
        data = r.content
        check_sha256(data, V15_SHA256, V15_REL)
        return data

    def rescue_helper(self) -> bytes:
        r = self._client.get(RESCUE_URL)
        r.raise_for_status()
        data = r.content
        check_sha256(data, RESCUE_SHA256, "wl82loader.bin")
        return data
