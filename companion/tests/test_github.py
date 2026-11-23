from __future__ import annotations

import httpx
import pytest

from drydock_sync.providers.github import GithubProvider

RELEASE = {
    "tag_name": "v1.2.3",
    "assets": [
        {"name": "skiff-1.2.3.fwsc", "size": 42, "browser_download_url": "https://example.com/skiff.fwsc"},
        {"name": "README.md", "size": 1, "browser_download_url": "https://example.com/README.md"},
    ],
}


def _client(status_code: int = 200) -> httpx.Client:
    def handler(request: httpx.Request) -> httpx.Response:
        if request.url.path == "/repos/o/r/releases/latest":
            return httpx.Response(status_code, json=RELEASE if status_code == 200 else None)
        if request.url.path == "/skiff.fwsc":
            return httpx.Response(200, content=b"skiff-data")
        return httpx.Response(404)

    return httpx.Client(transport=httpx.MockTransport(handler))


def test_latest_release_and_asset() -> None:
    g = GithubProvider(client=_client())
    release = g.latest_release("o/r")
    asset = g.find_asset(release, r"skiff-[\d.]+\.fwsc")
    assert asset["name"] == "skiff-1.2.3.fwsc"
    assert g.download_asset(asset) == b"skiff-data"
    g.close()


def test_asset_pattern_no_match_raises() -> None:
    g = GithubProvider(client=_client())
    release = g.latest_release("o/r")
    with pytest.raises(ValueError, match="asset"):
        g.find_asset(release, r"nope-.*")
    g.close()


def test_latest_release_404_raises() -> None:
    g = GithubProvider(client=_client(404))
    with pytest.raises(ValueError, match="release"):
        g.latest_release("o/r")
    g.close()
