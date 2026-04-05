import shutil
import os

src = r"C:\Users\hemsh_sfya5gq\.gemini\antigravity\scratch\quick_stt_app\QuickSTT_App\_internal"
dst = r"C:\Users\hemsh_sfya5gq\.gemini\antigravity\scratch\quick_stt_app\QuickSTT_Server\files\_internal"

if os.path.exists(src):
    print("Zipping _internal...")
    shutil.make_archive(dst, 'zip', src)
    print("Zipped successfully!")
else:
    print(f"Source {src} not found")
