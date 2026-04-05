# -*- mode: python ; coding: utf-8 -*-
# LEAN build: Only includes what's actually needed for STT + wakeword
# Dependencies: vosk, pyaudio, numpy, onnxruntime
# NO: torch, transformers, scipy, openwakeword (we use oww_lite.py instead)

from PyInstaller.utils.hooks import collect_data_files

datas = []
binaries = []
hiddenimports = [
    'onnxruntime',
    'numpy',
    'pyaudio',
    'requests', 'certifi', 'charset_normalizer', 'urllib3', 'idna',
    'tqdm',
]

# Collect vosk data files (native libs)
datas += collect_data_files('vosk')

# Collect OWW ONNX model files from openwakeword resources
import os, openwakeword
oww_resources = os.path.join(os.path.dirname(openwakeword.__file__), 'resources')
if os.path.isdir(oww_resources):
    datas += [(oww_resources, os.path.join('openwakeword', 'resources'))]

# Bundle oww_lite.py alongside the main scripts
datas += [('Source\\oww_lite.py', '.')]

a = Analysis(
    ['Source\\stt_service.py'],
    pathex=['Source'],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[
        # ML frameworks (not needed)
        'torch', 'torchaudio', 'torchvision',
        'tensorflow', 'tensorboard', 'tensorboardX',
        'faster_whisper', 'ctranslate2',
        'transformers', 'accelerate', 'peft', 'optimum',
        'datasets', 'huggingface_hub', 'tokenizers', 'safetensors', 'sentencepiece',
        # Scientific / data (not needed)
        'scipy', 'matplotlib', 'pandas', 'sklearn', 'scikit_learn',
        'librosa', 'numba', 'llvmlite', 'soxr',
        'h5py', 'pyarrow', 'cv2', 'PIL', 'Pillow',
        # Network / web frameworks (not needed)
        'flask', 'fastapi', 'uvicorn', 'starlette', 'werkzeug',
        'aiohttp', 'websockets', 'redis', 'sqlalchemy', 'psycopg2', 'asyncpg',
        'grpc', 'grpc_tools', 'protobuf',
        # 'requests',  # needed by vosk
        # Other bloat
        'av', 'pydub', 'IPython', 'nltk',
        'ddtrace', 'sentry_sdk',
        'opentelemetry_api', 'opentelemetry_sdk',
        'cryptography',
        'tiktoken', 'regex',
        'mcp', 'jiter', 'orjson', 'lxml', 'beautifulsoup4',
        'openwakeword',  # We use oww_lite.py directly, not the library
        # GUI (not needed in service)
        'tkinter', '_tkinter', 'tcl', 'tk',
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
    console=True,
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
