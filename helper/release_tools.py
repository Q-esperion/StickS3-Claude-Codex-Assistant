#!/usr/bin/env python3
"""Small release helpers for StickS3 firmware bundles."""

from __future__ import annotations

import hashlib
import json
import re
import shutil
from pathlib import Path


def read_macro(path: Path, name: str) -> str | None:
    if not path.exists():
        return None
    text = path.read_text(encoding="utf-8", errors="ignore")
    pattern = re.compile(r'^\s*#\s*define\s+' + re.escape(name) + r'\s+"([^"]*)"', re.MULTILINE)
    match = pattern.search(text)
    return match.group(1) if match else None


def project_version(project_root: Path) -> str:
    src = project_root / "src"
    for path in (src / "secrets.h", src / "remote_ota_config.h"):
        value = read_macro(path, "APP_VERSION")
        if value:
            return value
    return "0.0.0"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def build_manifest(version: str, firmware_url: str, firmware_path: Path, notes: str = "") -> dict:
    manifest = {
        "version": version,
        "url": firmware_url,
        "sha256": sha256_file(firmware_path),
        "size": firmware_path.stat().st_size,
    }
    if notes.strip():
        manifest["notes"] = notes.strip()
    return manifest


def write_release_bundle(
    firmware_path: Path,
    out_dir: Path,
    version: str,
    firmware_url: str,
    notes: str = "",
    helper_exe: Path | None = None,
) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    out_firmware = out_dir / "firmware.bin"
    shutil.copy2(firmware_path, out_firmware)

    manifest = build_manifest(version, firmware_url, out_firmware, notes)
    (out_dir / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )

    if helper_exe and helper_exe.exists():
        shutil.copy2(helper_exe, out_dir / helper_exe.name)

    return manifest
