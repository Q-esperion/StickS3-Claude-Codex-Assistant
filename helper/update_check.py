#!/usr/bin/env python3
"""Firmware update checks shared by the Windows helper UI."""

from __future__ import annotations

import json
import urllib.request


DEFAULT_MANIFEST_URL = (
    "https://github.com/bulb888/StickS3-Claude-Codex-Assistant/"
    "releases/latest/download/manifest.json"
)


def fetch_manifest(url: str, timeout: int = 8) -> dict:
    url = (url or "").strip()
    if not url:
        raise ValueError("manifest URL is empty")
    req = urllib.request.Request(
        url,
        headers={
            "Accept": "application/json",
            "User-Agent": "StickS3ClaudeCodexHelper",
        },
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read(1024 * 1024)
    data = json.loads(raw.decode("utf-8"))
    if not isinstance(data, dict):
        raise ValueError("manifest is not a JSON object")
    return data


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


def format_update_summary(manifest: dict) -> str:
    version = str(manifest.get("version") or "未知版本")
    lines = [f"最新固件: {version}"]
    size = format_size(manifest.get("size"))
    if size:
        lines.append(f"大小: {size}")
    notes = str(manifest.get("notes") or "").strip()
    if notes:
        lines.append(f"说明: {notes}")
    url = str(manifest.get("url") or "").strip()
    if url:
        lines.append(f"固件: {url}")
    return "\n".join(lines)
