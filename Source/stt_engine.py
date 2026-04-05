
import os
import sys
import threading
import json
import time
import wave
from datetime import datetime
import pyaudio
from vosk import Model, KaldiRecognizer

def log(msg): sys.stderr.write(f"[ENGINE] {msg}\n"); sys.stderr.flush()

# On frozen (PyInstaller) cold boot, Windows audio may not be ready.
# Wait a few seconds before touching PyAudio.
if getattr(sys, 'frozen', False):
    _boot_delay = 12  # Increased from 5 to 12 as per user request for Windows audio subsystem to boot
    log(f"Frozen mode detected — waiting {_boot_delay}s for audio subsystem...")
    time.sleep(_boot_delay)
    log("Boot delay complete.")

try:
    import numpy as np
except ImportError:
    np = None

# Import oww_lite at top level so PyInstaller bundles it as a module
try:
    import oww_lite
except ImportError:
    oww_lite = None

def get_root_dir():
    if getattr(sys, 'frozen', False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.abspath(__file__))

# Persistence & Portability Logic
def get_app_data_path():
    try:
        appdata = os.path.normpath(os.path.join(os.getenv("APPDATA"), "QuickSTT"))
        local_data = os.path.normpath(os.path.join(get_root_dir(), "data"))
        if os.path.exists(local_data):
            return local_data
        os.makedirs(appdata, exist_ok=True)
        return appdata
    except Exception as e:
        return get_root_dir()

APP_DATA_DIR = get_app_data_path()
MODELS_DIR = os.path.normpath(os.path.join(APP_DATA_DIR, "models"))
try:
    os.makedirs(MODELS_DIR, exist_ok=True)
except: pass

def _find_oww_models_dir():
    """Find the directory containing OWW ONNX models."""
    candidates = []
    root = get_root_dir()
    
    # 1. Bundled openwakeword resources inside _internal
    candidates.append(os.path.join(root, "_internal", "openwakeword", "resources", "models"))
    
    # 2. Local oww_models directory
    candidates.append(os.path.join(root, "oww_models"))
    
    # 3. Data directory
    candidates.append(os.path.join(root, "data", "oww_models"))
    
    for d in candidates:
        if os.path.isdir(d) and os.path.isfile(os.path.join(d, "melspectrogram.onnx")):
            return d
    return None

def load_settings():
    settings = {
        "wakeWords": ["hey jarvis", "alexa"],
        "closeWords": ["stop listening", "go to sleep"],
        "wakeEngine": "openwakeword (Default)"
    }
    if sys.platform == "win32":
        import winreg
        try:
            key_path = r"Software\QuickSTT\Config"
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key_path) as key:
                words, _ = winreg.QueryValueEx(key, "wakeWords")
                if isinstance(words, list):
                    settings["wakeWords"] = words
                elif isinstance(words, str):
                    settings["wakeWords"] = words.split(",")
                try:
                    cwords, _ = winreg.QueryValueEx(key, "closeWords")
                    if isinstance(cwords, list):
                        settings["closeWords"] = cwords
                    elif isinstance(cwords, str):
                        settings["closeWords"] = cwords.split(",")
                except: pass
                engine, _ = winreg.QueryValueEx(key, "wakeEngine")
                settings["wakeEngine"] = engine
                try:
                    recdir, _ = winreg.QueryValueEx(key, "recordingDir")
                    settings["recordingDir"] = recdir
                except: pass
        except: pass
    return settings

class STTEngine:
    def __init__(self, status_callback=None):
        self.status_callback = status_callback
        self.is_running = False
        self.active_engine = "Vosk Small En"
        self.active_model_path = ""
        self.model = None
        self.rec = None
        self.audio = None
        self.stream = None
        self.input_device_index = None
        self.mode = "IDLE"  # Start in IDLE so wakeword detection is active from boot
        
        # Initialize PyAudio with retries — critical for cold boot
        self._init_pyaudio()
        
        conf = load_settings()
        self.wake_words = conf["wakeWords"]
        self.close_words = conf.get("closeWords", ["stop listening"])
        self.wake_engine_name = conf.get("wakeEngine", "openwakeword (Default)")
        
        self.silence_limit = 15
        self.last_speech_time = time.time()
        self.wake_suppressed_until = 0.0
        self.last_activation_time = 0.0
        self._oww_hit_counts = {}
        self._vosk_wake_hits = 0
        self._wake_hit_requirement = 3

        # Recording & LRC
        self._recording_frames = []
        self._is_recording     = False
        self._recording_path   = ""
        self._lrc_events       = []
        self._recording_start_time = 0
        self.recording_dir     = conf.get("recordingDir") or os.path.join(APP_DATA_DIR, "recordings")

        # Watchdog tracking — prevent thread accumulation
        self._watchdog_running = False

    def _init_pyaudio(self):
        """Initialize PyAudio with retries for cold boot resilience."""
        max_retries = 10
        for attempt in range(1, max_retries + 1):
            try:
                if self.audio:
                    try:
                        self.audio.terminate()
                    except Exception:
                        pass
                self.audio = pyaudio.PyAudio()
                # Prefer the Windows default input device when it is available.
                found_input_index = None
                found_input = False
                for i in range(self.audio.get_device_count()):
                    try:
                        info = self.audio.get_device_info_by_index(i)
                        if info.get('maxInputChannels', 0) > 0:
                            found_input = True
                            if found_input_index is None:
                                found_input_index = i
                            break
                    except Exception:
                        continue
                if not found_input:
                    raise RuntimeError("No input audio devices found")

                self.input_device_index = found_input_index
                try:
                    default_info = self.audio.get_default_input_device_info()
                    if default_info and default_info.get('maxInputChannels', 0) > 0:
                        self.input_device_index = int(default_info.get('index'))
                except Exception as default_err:
                    log(f"Default input lookup failed: {default_err}")

                log(
                    f"PyAudio initialized OK on attempt {attempt} "
                    f"({self.audio.get_device_count()} devices, input index={self.input_device_index})"
                )
                return
            except Exception as e:
                log(f"PyAudio init failed (attempt {attempt}/{max_retries}): {e}")
                if self.status_callback:
                    self.status_callback(f"Audio init retry {attempt}/{max_retries}...", 3)
                time.sleep(3)
        log("CRITICAL: PyAudio init failed after all retries")
        if self.status_callback:
            self.status_callback("Audio Error — Retrying...", 3)

    def _start_recording(self, output_path=None):
        self._recording_frames = []
        self._lrc_events = []
        self._recording_start_time = time.time()
        self._is_recording = True

        if output_path:
            target_path = os.path.normpath(output_path)
            if not target_path.lower().endswith(".wav"):
                target_path += ".wav"
            rec_dir = os.path.dirname(target_path) or self.recording_dir
            file_name = os.path.basename(target_path)
        else:
            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            rec_dir = self.recording_dir
            file_name = f"rec_{timestamp}.wav"

        try:
            os.makedirs(rec_dir, exist_ok=True)
        except:
            rec_dir = os.path.join(APP_DATA_DIR, "recordings")
            os.makedirs(rec_dir, exist_ok=True)

        self._recording_path = os.path.join(rec_dir, file_name)
        log(f"Recording started -> {self._recording_path}")

    def _stop_recording(self, emit_saved_status=True):
        if not self._is_recording:
            return
        self._is_recording = False
        frames = self._recording_frames
        lrc_events = self._lrc_events
        self._recording_frames = []
        self._lrc_events = []
        if not frames:
            return
        path = self._recording_path
        try:
            with wave.open(path, 'wb') as wf:
                wf.setnchannels(1)
                wf.setsampwidth(2)
                wf.setframerate(16000)
                wf.writeframes(b''.join(frames))
            lrc_path = path.replace('.wav', '.lrc')
            try:
                with open(lrc_path, 'w', encoding='utf-8') as lf:
                    for offset, text in lrc_events:
                        m, s = divmod(offset, 60)
                        lf.write(f"[{int(m):02d}:{s:05.2f}] {text}\n")
                log(f"LRC saved: {lrc_path}")
            except Exception as le:
                log(f"LRC save failed: {le}")
            log(f"Recording saved: {path}")
            try:
                from pydub import AudioSegment
                mp3 = path.replace('.wav', '.mp3')
                AudioSegment.from_wav(path).export(mp3, format='mp3')
                os.remove(path)
                log(f"Converted to MP3: {mp3}")
                new_lrc = mp3.replace('.mp3', '.lrc')
                if os.path.exists(lrc_path) and lrc_path != new_lrc:
                    os.rename(lrc_path, new_lrc)
                if emit_saved_status and self.status_callback:
                    self.status_callback(f"Saved: {os.path.basename(mp3)}", 0)
            except Exception:
                if emit_saved_status and self.status_callback:
                    self.status_callback(f"Saved: {os.path.basename(path)}", 0)
        except Exception as e:
            log(f"Recording save failed: {e}")

    def start_manual_recording(self, output_path):
        self._start_recording(output_path)
        if self.mode == "ACTIVE" and self.status_callback:
            self.status_callback("Listening...", 1)

    def stop_manual_recording(self):
        self._stop_recording(emit_saved_status=False)
        if self.mode == "ACTIVE" and self.status_callback:
            self.status_callback("Listening...", 1)
        
    def start_loop(self):
        if self.is_running: return
        self.is_running = True
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        if not self._watchdog_running:
            self._watchdog_running = True
            threading.Thread(target=self._watchdog, daemon=True).start()

    def stop_loop(self):
        self.is_running = False
        if hasattr(self, "_thread") and self._thread.is_alive():
            self._thread.join(timeout=2.0)
        # Flush buffers
        self._oww_chunk_buffer = b""
        
    def set_engine(self, engine_name):
        self.active_engine = engine_name
        self.is_running = False 
        time.sleep(1) 
        self.start_loop()
        
    def toggle_listening(self):
        if self.mode == "ACTIVE":
            self.mode = "IDLE"
            self._suppress_wakeword(1.8)
            self._stop_recording()
            if self.status_callback: self.status_callback("Model Ready", 0)
        else:
            self.mode = "ACTIVE"
            self.last_speech_time = time.time()
            self._start_recording()
            if self.status_callback: self.status_callback("Listening...", 1)

    def force_pause(self):
        self.mode = "IDLE"
        self._suppress_wakeword(1.8)
        self._stop_recording()
        if self.status_callback: self.status_callback("Model Ready", 0)

    def force_sleep(self):
        self.mode = "SLEEP"
        self._suppress_wakeword(3.2)
        self._stop_recording()
        if self.status_callback: self.status_callback("Hidden", -1)

    def _suppress_wakeword(self, seconds):
        self.wake_suppressed_until = max(self.wake_suppressed_until, time.time() + seconds)
        self._oww_hit_counts.clear()
        self._vosk_wake_hits = 0
        try:
            if getattr(self, "oww_model", None):
                self.oww_model.reset()
        except Exception:
            pass
        try:
            if self.rec:
                self.rec.Reset()
        except Exception:
            pass

    def _get_model_path(self, name):
        if not os.path.exists(MODELS_DIR): return None
        for d in os.listdir(MODELS_DIR):
            f_path = os.path.join(MODELS_DIR, d)
            if not os.path.isdir(f_path): continue
            if "Small En" in name and "small-en" in d: return f_path
            if "Large En" in name and "en-us" in d and "small" not in d: return f_path
            if "Indian" in name and "en-in" in d: return f_path
            if "Small Cn" in name and "cn" in d and "small" in d: return f_path
            if "Large Cn" in name and "cn" in d and "small" not in d: return f_path
            if "Small Ru" in name and "ru" in d and "small" in d: return f_path
            if "Small Fr" in name and "fr" in d and "small" in d: return f_path
            if "Large Fr" in name and "fr" in d and "small" not in d: return f_path
            if "Small De" in name and "de" in d and "small" in d: return f_path
            if "Large De" in name and "de" in d and "small" not in d: return f_path
            if "Small Es" in name and "es" in d and "small" in d: return f_path
            if "Small Pt" in name and "pt" in d and "small" in d: return f_path
            if "Small It" in name and "it" in d and "small" in d: return f_path
            if "Small Ja" in name and "ja" in d and "small" in d: return f_path
            if d.lower() == name.lower(): return f_path
        return None

    def _ensure_model(self):
        path = self._get_model_path(self.active_engine)
        if path and os.path.exists(path):
            self.active_model_path = path
            return True
        return False

    def _run(self):
        log(f"Engine _run started, mode={self.mode}, wake_engine={self.wake_engine_name}")
        
        while True:
            if not self.is_running: break
            
            if not self._ensure_model():
                if self.status_callback: self.status_callback("Model Missing", 3)
                time.sleep(2)
                continue

            if self.status_callback: self.status_callback("Loading...", 3)
            log(f"Loading model from: {self.active_model_path}")
            
            try:
                self.model = Model(self.active_model_path)
                self.rec = KaldiRecognizer(self.model, 16000)
                log("Vosk model loaded OK")
                
                # Setup wakeword detector
                self.oww_model = None
                if "openwakeword" in self.wake_engine_name.lower():
                    try:
                        # Use our lightweight ONNX-only detector (no scipy needed!)
                        if oww_lite is None:
                            raise ImportError("oww_lite module not available")
                        
                        oww_dir = _find_oww_models_dir()
                        if oww_dir:
                            log(f"OWW models dir: {oww_dir}")
                            self.oww_model = oww_lite.LiteWakeWordDetector(
                                model_dir=oww_dir,
                                wake_words=self.wake_words,
                                threshold=0.60
                            )
                            log(f"LiteWakeWordDetector loaded successfully!")
                        else:
                            log("OWW models directory not found — falling back to vosk wakeword")
                    except Exception as e:
                        log(f"Failed to load LiteWakeWordDetector: {e}")
                        import traceback
                        traceback.print_exc()
                        self.oww_model = None
                else:
                    log(f"Wake engine is '{self.wake_engine_name}', not using OWW")

            except Exception as e:
                log(f"Model Load Error: {e}")
                if self.status_callback: self.status_callback("Load Error", 3)
                self.is_running = False
                return

            # Open microphone with retry — reinitialize PyAudio on repeated failures
            mic_ok = False
            for attempt in range(10):
                try:
                    if self.audio is None:
                        self._init_pyaudio()
                    stream_kwargs = dict(
                        format=pyaudio.paInt16,
                        channels=1,
                        rate=16000,
                        input=True,
                        frames_per_buffer=320, # Reduced for 20ms responsiveness
                    )
                    if self.input_device_index is not None:
                        stream_kwargs["input_device_index"] = self.input_device_index
                    self.stream = self.audio.open(**stream_kwargs)
                    self.stream.start_stream()
                    mic_ok = True
                    log(
                        f"Microphone opened on attempt {attempt+1} "
                        f"(input index={self.input_device_index})"
                    )
                    break
                except Exception as e:
                    log(f"Mic Error (attempt {attempt+1}/10): {e}")
                    if self.status_callback:
                        self.status_callback(f"Mic retry {attempt+1}/10...", 3)
                    # Every 3 failures, fully reinitialize PyAudio
                    if (attempt + 1) % 3 == 0:
                        log("Reinitializing PyAudio after repeated mic failures...")
                        try:
                            self.audio.terminate()
                        except Exception:
                            pass
                        self.audio = None
                        self._init_pyaudio()
                    time.sleep(3)
            
            if not mic_ok:
                log("Failed to open microphone after 10 attempts — will retry via watchdog")
                if self.status_callback: self.status_callback("Mic Error — Retrying...", 3)
                self.is_running = False
                # Don't return — let watchdog restart us
                return
            
            if self.mode == "SLEEP":
                if self.status_callback: self.status_callback("Hidden", -1)
            elif self.mode == "ACTIVE":
                if self.status_callback: self.status_callback("Listening...", 1)
            else:
                if self.status_callback: self.status_callback("Model Ready", 0)
            
            log(f"Entering main loop, mode={self.mode}, oww={'loaded' if self.oww_model else 'None'}")
            self.last_read_time = time.time()
            
            while self.is_running:
                try:
                    if self.stream is None or not self.stream.is_active():
                        raise RuntimeError("Microphone stream became inactive")

                    available = self.stream.get_read_available()
                    if available < 320:
                        time.sleep(0.005)
                        continue

                    data = self.stream.read(320, exception_on_overflow=False)
                    if len(data) == 0:
                        time.sleep(0.005)
                        continue
                    self.last_read_time = time.time()

                    if self._is_recording:
                        self._recording_frames.append(data)
                    
                    # Calculate audio level (every 20ms now)
                    level = 0
                    if np:
                        try:
                            audio_data = np.frombuffer(data, dtype=np.int16)
                            if len(audio_data) > 0:
                                rms = np.sqrt(np.mean(audio_data.astype(np.float32)**2))
                                if rms <= 0:
                                    level = 0
                                else:
                                    db = 20.0 * np.log10(rms)
                                    level = max(0, min(100, int((db - 35.0) / 50.0 * 100.0)))
                                sys.stdout.write(f"AUDIO_LEVEL|{level}\n")
                                sys.stdout.flush()
                        except Exception:
                            pass
                    
                    # 1. Wakeword detection (when IDLE or SLEEP)
                    if self.mode != "ACTIVE":
                        if time.time() < self.wake_suppressed_until:
                            continue
                        
                        # Accumulate for OWW (which needs 1280)
                        if not hasattr(self, "_oww_chunk_buffer"):
                            self._oww_chunk_buffer = b""
                        self._oww_chunk_buffer += data
                        
                        if len(self._oww_chunk_buffer) >= 2560: # 1280 samples * 2 bytes
                            process_data = self._oww_chunk_buffer[:2560]
                            self._oww_chunk_buffer = self._oww_chunk_buffer[2560:]
                            
                            if self.oww_model:
                                try:
                                    audio_frame = np.frombuffer(process_data, dtype=np.int16)
                                    prediction = self.oww_model.predict(audio_frame)
                                    for mdl, score in prediction.items():
                                        if score >= getattr(self.oww_model, "threshold", 0.60):
                                            self._oww_hit_counts[mdl] = self._oww_hit_counts.get(mdl, 0) + 1
                                        else:
                                            self._oww_hit_counts[mdl] = 0

                                        if self._oww_hit_counts[mdl] >= self._wake_hit_requirement and (time.time() - self.last_activation_time) > 2.0:
                                            log(f"OWW Detected: {mdl} score={score:.2f}")
                                            self._activate_stt()
                                            self._oww_chunk_buffer = b""
                                            break
                                except Exception as e:
                                    log(f"OWW predict error: {e}")
                            else:
                                # Vosk-based wakeword fallback (already works incrementally)
                                self.rec.AcceptWaveform(process_data)
                                partial_json = self.rec.PartialResult()
                                partial_text = json.loads(partial_json).get('partial', '').lower()
                                if partial_text and any(w.lower() in partial_text for w in self.wake_words):
                                    self._vosk_wake_hits += 1
                                    if self._vosk_wake_hits >= self._wake_hit_requirement and (time.time() - self.last_activation_time) > 2.0:
                                        log(f"Vosk wakeword match: '{partial_text}'")
                                        self._activate_stt()
                                        self.rec.Reset()
                                        self._oww_chunk_buffer = b""
                                else:
                                    self._vosk_wake_hits = 0
                        else:
                            # Not enough for OWW yet, but we can still feed Vosk partials for extra snappy feel?
                            # No, OWW is preferred if available.
                            pass
                    
                    # 2. Handle Dictation (if active)
                    else:
                        if level > 2:
                            self.last_speech_time = time.time()

                        if self.rec.AcceptWaveform(data):
                            res = json.loads(self.rec.Result())
                            text = res.get('text', '')
                            if text:
                                if any(cw.lower() in text.lower() for cw in self.close_words):
                                    log(f"Close word matched in final: '{text}'")
                                    self.force_sleep()
                                    continue
                                
                                if self._is_recording:
                                    offset = time.time() - self._recording_start_time
                                    self._lrc_events.append((offset, text))

                                self.last_speech_time = time.time()
                                self.last_partial = ""
                                sys.stdout.write(f"FINAL_TEXT|{text}\n")
                                sys.stdout.flush()
                                if self.status_callback: self.status_callback("Listening...", 1)
                        
                        # Handle partial results for live typing and close words
                        partial_json = self.rec.PartialResult()
                        partial_text = json.loads(partial_json).get('partial', '').strip()
                        if partial_text:
                            # 1. Check close words first
                            if any(cw.lower() in partial_text.lower() for cw in self.close_words):
                                log(f"Close word matched in partial: '{partial_text}'")
                                self.force_sleep()
                                self.rec.Reset()
                                self.last_partial = ""
                                continue
                            
                            # 2. Emit PARTIAL_TEXT for instantaneous UI feedback
                            if getattr(self, "last_partial", "") != partial_text:
                                self.last_partial = partial_text
                                sys.stdout.write(f"PARTIAL_TEXT|{partial_text}\n")
                                sys.stdout.flush()

                except Exception as e:
                    log(f"Loop Error: {e}")
                    time.sleep(0.1)

            if self.stream:
                try:
                    self.stream.stop_stream()
                    self.stream.close()
                except: pass
                self.stream = None

    def _activate_stt(self):
        self.mode = "ACTIVE"
        self.last_activation_time = time.time()
        self._oww_hit_counts.clear()
        self._vosk_wake_hits = 0
        self.last_speech_time = time.time()
        self._start_recording()
        if self.status_callback: self.status_callback("Wake Word!", 1)

    def _watchdog(self):
        """Single watchdog thread for the lifetime of the engine.
        Monitors for stalls AND ensures the engine restarts after failures."""
        log("Watchdog started")
        while True:
            time.sleep(3)
            # If the engine is not running (crashed, mic error, etc.), restart it
            if not self.is_running:
                log("Watchdog: Engine not running, restarting in 5s...")
                if self.status_callback:
                    self.status_callback("Restarting engine...", 3)
                time.sleep(5)
                self.start_loop()
                continue

            # If the engine IS running but stalled (no audio data for 10s)
            if self.is_running and (time.time() - getattr(self, 'last_read_time', time.time()) > 10):
                log("Watchdog: Engine stalled (no audio for 10s), terminating PyAudio to unblock thread and restarting...")
                self.is_running = False
                
                if self.stream:
                    try: self.stream.close()
                    except: pass
                    self.stream = None
                    
                if self.audio:
                    try: self.audio.terminate()
                    except: pass
                    self.audio = None
                    
                time.sleep(2)
                self.start_loop()

            # Auto-idle after silence_limit seconds in ACTIVE mode
            if self.mode == "ACTIVE" and (time.time() - self.last_speech_time > self.silence_limit):
                self.mode = "IDLE"
                self._suppress_wakeword(1.4)
                self._stop_recording()
                if self.status_callback: self.status_callback("Model Ready", 0)
