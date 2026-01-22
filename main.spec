# -*- mode: python ; coding: utf-8 -*-


a = Analysis(
    ['main.py'],
    pathex=[],
    binaries=[],
    datas=[('tablo2.png', '.'), ('baza.png', '.'), ('clock.png', '.'), ('esp32.bin', '.'), ('esp32_attendance.bin', '.'), ('C:\\Users\\tvcom\\PycharmProjects\\configurator_tablo\\.venv\\Lib\\site-packages\\esptool', 'esptool'), ('C:\\Users\\tvcom\\PycharmProjects\\configurator_tablo\\.venv\\Lib\\site-packages\\serial', 'serial'), ('C:\\Users\\tvcom\\PycharmProjects\\configurator_tablo\\.venv\\Lib\\site-packages\\cryptography', 'cryptography'), ('run_esptool.py', '.')],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='main',
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
    icon=['indicator.ico'],
)
