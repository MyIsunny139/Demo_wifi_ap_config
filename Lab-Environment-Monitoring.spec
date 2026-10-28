# -*- mode: python ; coding: utf-8 -*-

from pathlib import Path

from PyInstaller.utils.hooks import collect_all, collect_submodules


PROJECT_ROOT = Path(SPECPATH)
ultralytics_datas, ultralytics_binaries, ultralytics_hiddenimports = collect_all(
    "ultralytics"
)

datas = [
    (str(PROJECT_ROOT / "python_client" / "static"), "python_client/static"),
    (str(PROJECT_ROOT / "yolo11n.pt"), "."),
    *ultralytics_datas,
]
hiddenimports = sorted(
    set(
        ultralytics_hiddenimports
        + collect_submodules("uvicorn")
        + collect_submodules("websockets")
    )
)

analysis = Analysis(
    [str(PROJECT_ROOT / "python_client" / "packaged_launcher.py")],
    pathex=[str(PROJECT_ROOT)],
    binaries=ultralytics_binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(analysis.pure)

exe = EXE(
    pyz,
    analysis.scripts,
    [],
    exclude_binaries=True,
    name="Lab-Environment-Monitoring",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=True,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)

collection = COLLECT(
    exe,
    analysis.binaries,
    analysis.datas,
    strip=False,
    upx=False,
    upx_exclude=[],
    name="Lab-Environment-Monitoring",
)
