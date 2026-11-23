"""Releases GitHub directes : résout la release 'latest' et son asset .fwsc."""
from __future__ import annotations

import re
from typing import Any, cast

import httpx

API = "https://api.github.com"


class GithubProvider:
    def __init__(self, client: httpx.Client | None = None):
        self._client = client or httpx.Client(timeout=60)
        self._headers = {"Accept": "application/vnd.github+json"}

    def close(self) -> None:
        self._client.close()

    def latest_release(self, repo: str) -> dict[str, Any]:
        r = self._client.get(f"{API}/repos/{repo}/releases/latest", headers=self._headers)
        if r.status_code == 404:
            raise ValueError(f"{repo}: aucune release 'latest' (404)")
        r.raise_for_status()
        return cast(dict[str, Any], r.json())

    def find_asset(self, release: dict[str, Any], pattern: str) -> dict[str, Any]:
        for asset in release.get("assets", []):
            if re.search(pattern, asset["name"]):
                return cast(dict[str, Any], asset)
        names = ", ".join(a["name"] for a in release.get("assets", []))
        raise ValueError(
            f"asset '{pattern}' introuvable dans {release.get('tag_name', '?')} (assets: {names or 'aucun'})"
        )

    def download_asset(self, asset: dict[str, Any]) -> bytes:
        r = self._client.get(
            asset["browser_download_url"], headers={"Accept": "application/octet-stream"}
        )
        r.raise_for_status()
        return r.content
