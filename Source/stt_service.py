import sys
import os

def get_runtime_dir():
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.abspath(__file__))

# Keep startup logs beside the executable so cold-boot diagnostics do not
# depend on the desktop path or PyInstaller's _MEIPASS directory.
BASE_DIR = get_runtime_dir()
try:
    log_path = os.path.join(BASE_DIR, "stt_startup_log.txt")
    log_file = open(log_path, "a", encoding="utf-8")
    import datetime
    log_file.write(f"\\n\\n--- STT Service Startup {datetime.datetime.now()} ---\\n")
    log_file.flush()

    class DualWriter:
        def __init__(self, original, file):
            self.original = original
            self.file = file
        def write(self, text):
            if self.original:
                try: self.original.write(text)
                except: pass
            try:
                self.file.write(text)
                self.file.flush()
            except: pass
        def flush(self):
            if self.original:
                try: self.original.flush()
                except: pass
            try: self.file.flush()
            except: pass

    sys.stdout = DualWriter(sys.stdout, log_file)
    sys.stderr = DualWriter(sys.stderr, log_file)
except Exception:
    pass

# Force CWD to the executable directory before importing the engine.
try:
    os.chdir(BASE_DIR)
except Exception:
    pass

import json
import time
import threading
import urllib.request
import zipfile
import shutil
import traceback

# Lazy load whisper to speed up start
faster_whisper = None 

from stt_engine import STTEngine, APP_DATA_DIR, MODELS_DIR

VOSK_MODELS = {
    "Vosk Small En": "https://alphacephei.com/vosk/models/vosk-model-small-en-us-0.15.zip",
    "Vosk Large En": "https://alphacephei.com/vosk/models/vosk-model-en-us-0.22.zip",
    "Vosk Indian En": "https://alphacephei.com/vosk/models/vosk-model-en-in-0.5.zip",
    "Vosk Small Cn": "https://alphacephei.com/vosk/models/vosk-model-small-cn-0.22.zip",
    "Vosk Large Cn": "https://alphacephei.com/vosk/models/vosk-model-cn-0.22.zip",
    "Whisper Tiny": "https://huggingface.co/guillaumekln/faster-whisper-tiny",
    "Whisper Turbo": "https://huggingface.co/deepdml/faster-whisper-large-v3-turbo-ct2",
}

def send_event(evt_type, data=""):
    try:
        sys.stdout.write(f"{evt_type}|{data}\n")
        sys.stdout.flush()
    except: pass

class ServiceHandle:
    def __init__(self):
        send_event("STATE", "3,Starting engine...")
        self.engine = STTEngine(status_callback=self.on_status)
        self.download_thread = None

        self.engine.start_loop()
        
        t = threading.Thread(target=self.input_loop, daemon=True)
        t.start()

    def run_forever(self):
        """Block the main thread. Called externally after __init__."""
        try:
            while True: time.sleep(1)
        except KeyboardInterrupt: sys.exit(0)

    def on_status(self, text, code):
        send_event("STATE", f"{code},{text}")

    def input_loop(self):
        for line in sys.stdin:
            try:
                cmd = line.strip()
                if not cmd: continue
                parts = cmd.split(":", 1)
                action = parts[0].upper()
                payload = parts[1] if len(parts) > 1 else ""

                if action == "TOGGLE": self.engine.toggle_listening()
                elif action == "STOP": self.engine.force_pause()
                elif action == "SLEEP": self.engine.force_sleep()
                elif action == "MODEL": self.switch_model(payload)
                elif action == "DOWNLOAD": self.start_download(payload)
                elif action == "RECORD_START": self.engine.start_manual_recording(payload)
                elif action == "RECORD_STOP": self.engine.stop_manual_recording()
                elif action == "UNINSTALL": self.perform_uninstall(payload)
                elif action == "WAKEWORDS":
                    new_words = [w.strip() for w in payload.split(",") if w.strip()]
                    self.engine.wake_words = new_words
                    send_event("STATE", "3,Reloading wakeword engine...")
                    self.engine.stop_loop()
                    time.sleep(0.5)
                    self.engine.start_loop()
                    send_event("STATE", "0,Wakewords updated")
                elif action == "CLOSEWORDS":
                    new_words = [w.strip() for w in payload.split(",") if w.strip()]
                    self.engine.close_words = new_words
                    send_event("STATE", "0,Close words updated")
                elif action == "WAKEMODE":
                    self.engine.wake_engine_name = payload
                    send_event("STATE", "3,Switching wake engine...")
                    self.engine.stop_loop()
                    time.sleep(0.5)
                    self.engine.start_loop()
                    send_event("STATE", "0,Wake engine updated")
                elif action == "SET_REC_DIR":
                    self.engine.recording_dir = payload
                    import winreg
                    try:
                        key_path = r"Software\QuickSTT\Config"
                        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, key_path) as key:
                            winreg.SetValueEx(key, "recordingDir", 0, winreg.REG_SZ, payload)
                    except: pass
                elif action == "OFFLOAD":
                    send_event("STATE", "3,Offloading model...")
                    self.engine.stop_loop()
                    self.engine.model = None
                    self.engine.rec = None
                    self.engine.oww_model = None
                    import gc; gc.collect()
                    send_event("STATE", "0,Model offloaded")
                    print("Model offloaded to save RAM")
                elif action == "RELOAD":
                    send_event("STATE", "3,Reloading model...")
                    self.engine.start_loop()
                    print("Model reload requested")
                elif action == "QUIT": sys.exit(0)
            except Exception as e:
                send_event("ERROR", str(e))

    def switch_model(self, model_name):
        model_name = model_name.strip()
        if "Whisper" in model_name:
            send_event("ERROR", "Whisper runtime support is not ready yet")
            send_event("STATE", "3,Whisper support is coming soon")
            return
        self.engine.set_engine(model_name)

    def start_download(self, model_name):
        model_name = model_name.strip()
        if self.download_thread and self.download_thread.is_alive():
            send_event("ERROR", "Another model download is already running")
            return
        
        if "Whisper" in model_name:
             self.download_thread = threading.Thread(target=self._download_whisper, args=(model_name,), daemon=True)
        else:
             url = VOSK_MODELS.get(model_name)
             if url:
                 self.download_thread = threading.Thread(target=self._download_vosk, args=(model_name, url), daemon=True)
             else:
                 send_event("DL_ERROR", f"No direct download configured for {model_name}")
                 send_event("DL_COMPLETE", "Failed")
        
        if self.download_thread: self.download_thread.start()

    def _download_vosk(self, name, url):
        try:
            zip_path = os.path.join(APP_DATA_DIR, "temp.zip")
            send_event("DL_START", name)
            
            try:
                import requests
                response = requests.get(url, stream=True, timeout=30)
                total_size = int(response.headers.get('content-length', 0))
                downloaded = 0
                
                with open(zip_path, 'wb') as f:
                    for chunk in response.iter_content(chunk_size=1024*1024):
                        if chunk:
                            f.write(chunk)
                            downloaded += len(chunk)
                            if total_size > 0:
                                pct = int((downloaded / total_size) * 100)
                                send_event("DL_PROGRESS", str(pct))
            except ImportError:
                def report(block, size, total):
                    if total > 0:
                        pct = int((block * size * 100) / total)
                        if pct > 100: pct = 100
                        send_event("DL_PROGRESS", str(pct))
                urllib.request.urlretrieve(url, zip_path, report)

            send_event("DL_STATUS", "Unpacking...")
            with zipfile.ZipFile(zip_path, 'r') as z:
                z.extractall(MODELS_DIR)
            
            os.remove(zip_path)
            send_event("DL_COMPLETE", name)
            self.switch_model(name)
        except Exception as e: 
            send_event("DL_ERROR", str(e))
            send_event("DL_COMPLETE", "Failed")

    def _download_whisper(self, name):
        send_event("DL_ERROR", f"{name} is listed, but Whisper downloads are not ready in this build")
        send_event("DL_COMPLETE", "Failed")

    def perform_uninstall(self, name):
        try:
            from stt_engine import MODELS_DIR
            targets = []
            if "Small En" in name: targets = ["vosk-model-small-en-us"]
            elif "Large En" in name: targets = ["vosk-model-en-us"]
            
            for d in os.listdir(MODELS_DIR):
                if name.lower() in d.lower() or any(t.lower() in d.lower() for t in targets):
                    shutil.rmtree(os.path.join(MODELS_DIR, d))
                    send_event("UNINSTALL_COMPLETE", name)
                    return
            send_event("ERROR", f"Model folder not found for {name}")
        except Exception as e:
            send_event("ERROR", f"Uninstall Failed: {str(e)}")

if __name__ == "__main__":
    send_event("INIT", "Service Ready")
    h = ServiceHandle()
    # Now block forever (separated from constructor)
    h.run_forever()
