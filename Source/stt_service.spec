# -*- mode: python ; coding: utf-8 -*-


a = Analysis(
    ['stt_service.py'],
    pathex=[],
    binaries=[],
    datas=[],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[
        'torch', 'torchvision', 'torchaudio', 'cv2', 'numpy', 'pandas', 'matplotlib', 
        'scipy', 'sklearn', 'IPython', 'Cython', 'jedi', 'black', 'datasets',
        'onnxruntime', 'transformers', 'pyarrow', 'llvmlite', 'h5py'
    ],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name='stt_service',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=True,
    upx_exclude=[],
    name='stt_service',
)
