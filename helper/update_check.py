#!/usr/bin/env python3
"""Assistant update checks shared by the Windows helper UI."""

from __future__ import annotations

import json
import re
import urllib.request


DEFAULT_REPO = "bulb888/StickS3-Claude-Codex-Assistant"
DEFAULT_RELEASE_API_URL = f"https://api.github.com/repos/{DEFAULT_REPO}/releases/latest"
HELPER_ASSET_NAME = "StickS3ClaudeCodexHelper.exe"
HELPER_RELEASE_PAGE = (
    "https://github.com/bulb888/StickS3-Claude-Codex-Assistant/releases/latest"
)


def fetch_latest_release(url: str = DEFAULT_RELEASE_API_URL, timeout: int = 8) -> dict:
    url = (url or "").strip()
    if not url:
        raise ValueError("release API URL is empty")
    req = urllib.request.Request(
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "StickS3ClaudeCodexHelper",
        },
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read(2 * 1024 * 1024)
    data = json.loads(raw.decode("utf-8"))
    if not isinstance(data, dict):
        raise ValueError("release response is not a JSON object")
    return data


def normalized_version(version: object) -> str:
    text = str(version or "").strip()
    if text.startswith(("v", "V")):
        text = text[1:]
    return text


def version_key(version: object) -> tuple[int, ...] | None:
    text = normalized_version(version)
    if not re.fullmatch(r"\d+(?:\.\d+)*", text):
        return None
    return tuple(int(part) for part in text.split("."))


def is_newer_version(latest: object, current: object) -> bool | None:
    latest_key = version_key(latest)
    current_key = version_key(current)
    if latest_key is None or current_key is None:
        return None
    width = max(len(latest_key), len(current_key))
    latest_key += (0,) * (width - len(latest_key))
    current_key += (0,) * (width - len(current_key))
    return latest_key > current_key


def find_asset(release: dict, name: str = HELPER_ASSET_NAME) -> dict | None:
    assets = release.get("assets") or []
    if not isinstance(assets, list):
        return None
    for asset in assets:
        if isinstance(asset, dict) and asset.get("name") == name:
            return asset
    return None


def format_size(size: object) -> str:
    try:
        n = int(size)
    except (TypeError, ValueError):
        return ""
    if n <= 0:
        return ""
    if n >= 1024 * 1024:
        return f"{n / (1024 * 1024):.1f} MB"
    if n >= 1024:
        return f"{n // 1024} KB"
    return f"{n} B"


def format_helper_update_summary(
    release: dict,
    current_version: str,
    asset_name: str = HELPER_ASSET_NAME,
) -> str:
    latest_tag = str(release.get("tag_name") or release.get("name") or "未知版本")
    latest_version = normalized_version(latest_tag)
    current = normalized_version(current_version)
    lines = [
        f"当前助手: {current or '未知'}",
        f"最新助手: {latest_version or latest_tag}",
    ]
    newer = is_newer_version(latest_version, current)
    if newer is True:
        lines.append("状态: 发现新版")
    elif newer is False:
        lines.append("状态: 已是最新版")

    asset = find_asset(release, asset_name)
    if not asset:
        lines.append(f"未找到资产: {asset_name}")
        page = str(release.get("html_url") or HELPER_RELEASE_PAGE)
        lines.append(f"发布页: {page}")
        return "\n".join(lines)

    size = format_size(asset.get("size"))
    if size:
        lines.append(f"大小: {size}")
    notes = str(release.get("body") or "").strip()
    if notes:
        first_line = notes.splitlines()[0].strip()
        if first_line:
            lines.append(f"说明: {first_line[:120]}")
    url = str(asset.get("browser_download_url") or "").strip()
    if url:
        lines.append(f"下载: {url}")
    return "\n".join(lines)
