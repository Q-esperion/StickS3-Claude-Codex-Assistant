# -*- mode: python ; coding: utf-8 -*-

import os

block_cipher = None

# Absolute path to helper dir (spec execution context has no __file__)
_helper_dir = os.path.abspath(SPECPATH)
_icon_file = os.path.join(_helper_dir, "icon_64.png")

a = Analysis(
    ['type_server_ubuntu.py'],
    pathex=[_helper_dir],
    binaries=[],
    datas=[
        (os.path.join(_helper_dir, "config_ubuntu.json"), "."),
        (os.path.join(_helper_dir, "status_logic.py"), "."),
        (os.path.join(_helper_dir, "version_info.py"), "."),
        (os.path.join(_helper_dir, "icon_64.png"), "."),
    ],
    hiddenimports=['status_logic', 'version_info', 'gi', 'gi.repository'],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=['tkinter'],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure, zlib_data_only=True, cipher=block_cipher)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='StickS3HelperUbuntu',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=_icon_file,
)