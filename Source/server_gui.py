import tkinter as tk
from tkinter import scrolledtext
import threading
import subprocess
import os
import sys
import time

class ServerGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("QuickSTT - Global Update Server")
        self.root.geometry("600x450")
        
        # The server dir is where THIS exe lives (QuickSTT_Server/)
        if getattr(sys, 'frozen', False):
            self.server_dir = os.path.dirname(sys.executable)
        else:
            self.server_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
            server_candidate = os.path.join(self.server_dir, "QuickSTT_Server")
            if os.path.exists(server_candidate):
                self.server_dir = server_candidate
        
        self.base_dir = os.path.dirname(self.server_dir)
        
        # UI
        self.btn_frame = tk.Frame(root)
        self.btn_frame.pack(pady=10)
        
        self.start_btn = tk.Button(self.btn_frame, text="Start Server", command=self.start_server, bg="green", fg="white", width=20)
        self.start_btn.pack(side=tk.LEFT, padx=5)
        
        self.stop_btn = tk.Button(self.btn_frame, text="Stop Server", command=self.stop_server, bg="red", fg="white", width=20, state=tk.DISABLED)
        self.stop_btn.pack(side=tk.LEFT, padx=5)
        
        self.log_area = scrolledtext.ScrolledText(root, width=70, height=20)
        self.log_area.pack(padx=10, pady=5)
        
        self.info_label = tk.Label(root, text=f"Server Dir: {self.server_dir}", fg="blue")
        self.info_label.pack(pady=5)
        
        self.server_process = None
        self.log(f"Ready. Server dir: {self.server_dir}")
        self.log(f"Files dir: {os.path.join(self.server_dir, 'files')}")
        self.log(f"Manifest: {os.path.join(self.server_dir, 'manifest_txt')}")

    def log(self, msg):
        self.log_area.insert(tk.END, f"[{time.strftime('%H:%M:%S')}] {msg}\n")
        self.log_area.see(tk.END)

    def start_server(self):
        self.log("Launching Backend...")
        
        # Find server_backend.py
        backend_path = os.path.join(self.base_dir, "Source", "server_backend.py")
        if not os.path.exists(backend_path):
            # Fallback: look next to ourselves
            backend_path = os.path.join(self.server_dir, "server_backend.py")
        
        if not os.path.exists(backend_path):
            self.log(f"[ERROR] Cannot find server_backend.py!")
            self.log(f"  Looked in: {os.path.join(self.base_dir, 'Source')}")
            self.log(f"  Looked in: {self.server_dir}")
            return
        
        self.log(f"Using backend: {backend_path}")
        
        # CRITICAL: Set cwd to QuickSTT_Server so Flask finds manifest_txt and files/
        self.server_process = subprocess.Popen(
            [sys.executable, backend_path], 
            stdout=subprocess.PIPE, 
            stderr=subprocess.STDOUT,
            text=True,
            cwd=self.server_dir  # THIS IS THE FIX
        )
        
        self.start_btn.config(state=tk.DISABLED)
        self.stop_btn.config(state=tk.NORMAL)
        
        threading.Thread(target=self.pull_logs, daemon=True).start()

    def pull_logs(self):
        for line in self.server_process.stdout:
            self.log(line.strip())
        self.log("Server stopped.")
        self.start_btn.config(state=tk.NORMAL)
        self.stop_btn.config(state=tk.DISABLED)

    def stop_server(self):
        if self.server_process:
            self.server_process.terminate()
            self.log("Stopping server...")

if __name__ == "__main__":
    root = tk.Tk()
    gui = ServerGUI(root)
    root.mainloop()
