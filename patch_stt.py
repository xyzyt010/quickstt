import os

path = r"Source\stt_engine.py"
with open(path, 'r', encoding='utf-8') as f:
    lines = f.readlines()

new_lines = []
for line in lines:
    if "keyboard.write(text + \" \")" in line:
        # Find the line "if text:"
        # Let me just replace the block.
        pass

# Actually, let's find the Exact index.
content = "".join(lines)
old_block = """                                 res = json.loads(self.rec.Result())
                                 text = res.get('text', '')
                                 if text:
                                     self.last_speech_time = time.time()
                                     if self.status_callback: self.status_callback(f"Typing: {text}", 2)
                                     keyboard.write(text + " ")
                                     time.sleep(0.05)
                                     if self.status_callback: self.status_callback("Listening...", 1)"""

# I will use a regex safely.
import re
pattern = r"(\s+)res = json\.loads\(self\.rec\.Result\(\)\)\n(\s+)text = res\.get\('text', ''\)\n(\s+)if text:\n(\s+)self\.last_speech_time = time\.time\(\)\n(\s+)if self\.status_callback: self\.status_callback\(f\"Typing: \{text\}\", 2\)\n(\s+)keyboard\.write\(text \+ \" \"\)\n(\s+)time\.sleep\(0\.05\)\n(\s+)if self\.status_callback: self\.status_callback\(\"Listening\.\.\.\", 1\)"

replacement = r"\1res = json.loads(self.rec.Result())\n\2text = res.get('text', '')\n\3if text:\n\4# Capture for LRC if recording\n\4if self._is_recording:\n\4    offset = time.time() - self._recording_start_time\n\4    self._lrc_events.append((offset, text))\n\n\4self.last_speech_time = time.time()\n\5if self.status_callback: self.status_callback(f\"Typing: {text}\", 2)\n\6keyboard.write(text + \" \")\n\7time.sleep(0.05)\n\8if self.status_callback: self.status_callback(\"Listening...\", 1)"

new_content = re.sub(pattern, replacement, content)

with open(path, 'w', encoding='utf-8') as f:
    f.write(new_content)
print("Updated stt_engine.py")
