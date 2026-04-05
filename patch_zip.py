import zipfile
with zipfile.ZipFile(r"C:\Users\hemsh_sfya5gq\.gemini\antigravity\scratch\quick_stt_app\QuickSTT_Server\files\_internal.zip", "a") as z:
    z.writestr("_internal/setuptools/_vendor/jaraco/text/Lorem ipsum.txt", "")
print("Patched _internal.zip")
