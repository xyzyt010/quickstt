"""
Lightweight OpenWakeWord detector using ONNX Runtime directly.
No dependency on the openwakeword Python library (avoids scipy requirement).
Uses the same ONNX model files but runs inference directly.
"""
import os
import sys
import numpy as np
from collections import deque

def log(msg): sys.stderr.write(f"[OWW-LITE] {msg}\n"); sys.stderr.flush()

class LiteWakeWordDetector:
    """
    Runs the OpenWakeWord inference pipeline directly via onnxruntime:
      1. Audio → mel spectrogram (melspectrogram.onnx)
      2. Mel spectrogram → embedding (embedding_model.onnx)
      3. Embedding → wakeword score (e.g., hey_jarvis_v0.1.onnx)
    
    No scipy, no openwakeword library import needed.
    """
    
    # Map of friendly names to OWW model filenames
    BUILTIN_MAP = {
        "computer":    "computer_v0.1.onnx",
        "hey jarvis":  "hey_jarvis_v0.1.onnx",
        "hey mycroft": "hey_mycroft_v0.1.onnx",
        "alexa":       "alexa_v0.1.onnx",
        "hey rhasspy": "hey_rhasspy_v0.1.onnx",
        "timer":       "timer_v0.1.onnx",
        "weather":     "weather_v0.1.onnx",
    }
    
    def __init__(self, model_dir, wake_words, threshold=0.4):
        """
        Args:
            model_dir: Directory containing the ONNX model files
            wake_words: List of wake word names (e.g., ["hey jarvis", "alexa"])
            threshold: Detection threshold (0.0-1.0)
        """
        import onnxruntime as ort
        
        self.threshold = threshold
        self.models = {}
        
        # ONNX session options for lightweight inference
        opts = ort.SessionOptions()
        opts.inter_op_num_threads = 1
        opts.intra_op_num_threads = 1
        opts.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
        providers = ["CPUExecutionProvider"]
        
        # Load melspectrogram model
        melspec_path = os.path.join(model_dir, "melspectrogram.onnx")
        if not os.path.isfile(melspec_path):
            raise FileNotFoundError(f"Melspectrogram model not found: {melspec_path}")
        self.melspec_model = ort.InferenceSession(melspec_path, sess_options=opts, providers=providers)
        log(f"Loaded melspectrogram model")
        
        # Load embedding model
        embed_path = os.path.join(model_dir, "embedding_model.onnx")
        if not os.path.isfile(embed_path):
            raise FileNotFoundError(f"Embedding model not found: {embed_path}")
        self.embedding_model = ort.InferenceSession(embed_path, sess_options=opts, providers=providers)
        log(f"Loaded embedding model")
        
        # Load wakeword models
        for w in wake_words:
            key = w.lower().strip()
            fname = self.BUILTIN_MAP.get(key)
            if not fname:
                log(f"No built-in OWW model for '{w}' — skipping")
                continue
            fpath = os.path.join(model_dir, fname)
            if not os.path.isfile(fpath) or os.path.getsize(fpath) < 10240:
                log(f"OWW model missing/corrupt: {fpath} — skipping '{w}'")
                continue
            
            session = ort.InferenceSession(fpath, sess_options=opts, providers=providers)
            n_input_features = session.get_inputs()[0].shape[1]
            n_outputs = session.get_outputs()[0].shape[1]
            model_name = os.path.splitext(fname)[0]
            self.models[model_name] = {
                "session": session,
                "n_features": n_input_features,
                "n_outputs": n_outputs,
            }
            log(f"Loaded wakeword model: {fname} (input={n_input_features}, output={n_outputs})")
        
        if not self.models:
            log("WARNING: No valid wakeword models loaded — detector will be inactive")
            self.prediction_buffer = {}
            return
        
        # Audio buffers (same approach as openwakeword AudioFeatures)
        self.raw_data_buffer = deque(maxlen=16000 * 10)  # 10 seconds
        self.melspectrogram_buffer = np.ones((76, 32), dtype=np.float32)
        self.melspectrogram_max_len = 10 * 97  # ~10 seconds
        self.accumulated_samples = 0
        self.raw_data_remainder = np.empty(0)
        
        # Initialize feature buffer with random data
        self.feature_buffer = self._get_embeddings(
            np.random.randint(-1000, 1000, 16000 * 4).astype(np.int16)
        )
        self.feature_buffer_max_len = 120
        
        # Prediction buffer for smoothing
        self.prediction_buffer = {m: deque(maxlen=30) for m in self.models}
        
        log(f"LiteWakeWordDetector ready with {len(self.models)} model(s)")
    
    def _get_melspectrogram(self, x):
        """Compute mel spectrogram from raw int16 audio."""
        x = np.array(x).astype(np.int16) if isinstance(x, list) else x
        if len(x.shape) < 2:
            x = x[None, ]
        x = x.astype(np.float32)
        outputs = self.melspec_model.run(None, {'input': x})
        spec = np.squeeze(outputs[0])
        # Transform to match Google's speech_embedding expected input
        spec = spec / 10 + 2
        return spec
    
    def _get_embeddings(self, x):
        """Compute embeddings from raw audio."""
        spec = self._get_melspectrogram(x)
        windows = []
        for i in range(0, spec.shape[0], 8):
            window = spec[i:i+76]
            if window.shape[0] == 76:
                windows.append(window)
        if not windows:
            return np.zeros((1, 96), dtype=np.float32)
        batch = np.expand_dims(np.array(windows), axis=-1).astype(np.float32)
        embedding = self.embedding_model.run(None, {'input_1': batch})[0].squeeze()
        if len(embedding.shape) == 1:
            embedding = embedding[None, ]
        return embedding
    
    def _streaming_features(self, x):
        """Process a chunk of audio and update internal feature buffers."""
        processed_samples = 0
        
        if self.raw_data_remainder.shape[0] != 0:
            x = np.concatenate((self.raw_data_remainder, x))
            self.raw_data_remainder = np.empty(0)
        
        if self.accumulated_samples + x.shape[0] >= 1280:
            remainder = (self.accumulated_samples + x.shape[0]) % 1280
            if remainder != 0:
                x_even = x[:-remainder]
                self.raw_data_buffer.extend(x_even.tolist())
                self.accumulated_samples += len(x_even)
                self.raw_data_remainder = x[-remainder:]
            else:
                self.raw_data_buffer.extend(x.tolist())
                self.accumulated_samples += x.shape[0]
                self.raw_data_remainder = np.empty(0)
        else:
            self.accumulated_samples += x.shape[0]
            self.raw_data_buffer.extend(x.tolist() if isinstance(x, np.ndarray) else x)
        
        if self.accumulated_samples >= 1280 and self.accumulated_samples % 1280 == 0:
            # Compute mel spectrogram
            n_samples = self.accumulated_samples
            raw_list = list(self.raw_data_buffer)
            audio_chunk = np.array(raw_list[-n_samples - 160*3:], dtype=np.int16)
            mel = self._get_melspectrogram(audio_chunk)
            self.melspectrogram_buffer = np.vstack((self.melspectrogram_buffer, mel))
            if self.melspectrogram_buffer.shape[0] > self.melspectrogram_max_len:
                self.melspectrogram_buffer = self.melspectrogram_buffer[-self.melspectrogram_max_len:]
            
            # Compute embeddings
            for i in np.arange(self.accumulated_samples // 1280 - 1, -1, -1):
                ndx = -8 * i
                ndx = ndx if ndx != 0 else len(self.melspectrogram_buffer)
                x_mel = self.melspectrogram_buffer[-76 + ndx:ndx].astype(np.float32)[None, :, :, None]
                if x_mel.shape[1] == 76:
                    emb = self.embedding_model.run(None, {'input_1': x_mel})[0].squeeze()
                    if len(emb.shape) == 1:
                        emb = emb[None, ]
                    self.feature_buffer = np.vstack((self.feature_buffer, emb))
            
            processed_samples = self.accumulated_samples
            self.accumulated_samples = 0
        
        if self.feature_buffer.shape[0] > self.feature_buffer_max_len:
            self.feature_buffer = self.feature_buffer[-self.feature_buffer_max_len:]
        
        return processed_samples if processed_samples != 0 else self.accumulated_samples
    
    def predict(self, audio_int16):
        """
        Run wakeword detection on a chunk of int16 audio (e.g., 1280 samples = 80ms @ 16kHz).
        
        Args:
            audio_int16: numpy array of int16 audio samples
            
        Returns:
            dict: {model_name: score} for each loaded wakeword model
        """
        if not self.models:
            return {}
        n_prepared = self._streaming_features(audio_int16)
        
        predictions = {}
        for mdl_name, mdl_info in self.models.items():
            session = mdl_info["session"]
            n_features = mdl_info["n_features"]
            n_outputs = mdl_info["n_outputs"]
            
            if n_prepared >= 1280:
                # Get features for this model
                features = self.feature_buffer[-n_features:][None, ].astype(np.float32)
                if features.shape[1] == n_features:
                    result = session.run(None, {session.get_inputs()[0].name: features})
                    if n_outputs == 1:
                        score = float(result[0][0][0])
                    else:
                        score = float(np.max(result[0][0]))
                    predictions[mdl_name] = score
                else:
                    predictions[mdl_name] = 0.0
            else:
                # Not enough samples yet
                if len(self.prediction_buffer[mdl_name]) > 0:
                    predictions[mdl_name] = self.prediction_buffer[mdl_name][-1]
                else:
                    predictions[mdl_name] = 0.0
            
            # Zero out first 5 predictions during initialization
            if len(self.prediction_buffer[mdl_name]) < 5:
                predictions[mdl_name] = 0.0
            
            self.prediction_buffer[mdl_name].append(predictions[mdl_name])
        
        return predictions
    
    def reset(self):
        """Reset all buffers."""
        self.raw_data_buffer.clear()
        self.melspectrogram_buffer = np.ones((76, 32), dtype=np.float32)
        self.accumulated_samples = 0
        self.raw_data_remainder = np.empty(0)
        self.feature_buffer = self._get_embeddings(
            np.random.randint(-1000, 1000, 16000 * 4).astype(np.int16)
        )
        self.prediction_buffer = {m: deque(maxlen=30) for m in self.models}
