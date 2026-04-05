import os
import sys
import json
import ctypes
import subprocess
import threading
import tkinter as tk
from tkinter import scrolledtext

from flask import Flask, send_from_directory, jsonify, send_file
from waitress import serve

app_flask = Flask(__name__)

def find_server_dir():
    cwd = os.getcwd()
    if os.path.exists(os.path.join(cwd, "files")) and os.path.exists(os.path.join(cwd, "manifest_txt")):
        return cwd
    parent = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    s_dir = os.path.join(parent, "QuickSTT_Server")
    if os.path.exists(s_dir):
        return s_dir
    script_dir = os.path.dirname(os.path.abspath(__file__))
    s_dir = os.path.join(script_dir, "QuickSTT_Server")
    if os.path.exists(s_dir):
        return s_dir
    return cwd

SERVER_DIR = find_server_dir()
FILES_DIR = os.path.join(SERVER_DIR, "files")
VERSION_FILE = os.path.join(SERVER_DIR, "version.json")
MANIFEST_FILE = os.path.join(SERVER_DIR, "manifest_txt")

@app_flask.route('/')
def index():
    html = f"""<html><head><title>QuickSTT Update Server</title>
    <style>body{{background:#1a1a1a;color:#eee;font-family:'Segoe UI',sans-serif;display:flex;justify-content:center;align-items:center;height:100vh;margin:0}}
    .c{{background:#2a2a2a;padding:40px;border-radius:12px;box-shadow:0 4px 20px rgba(0,0,0,.6);text-align:center}}
    h1{{color:#38bdf8;margin-top:0}}p{{color:#aaa}}</style></head>
    <body><div class="c"><h1>QuickSTT Server v1.0</h1><p>Status: <b style="color:#4ade80">Active</b></p>
    <p>Serving from:<br><small style="color:#666">{SERVER_DIR}</small></p></div></body></html>"""
    return html

@app_flask.route('/version')
def get_version():
    if os.path.exists(VERSION_FILE):
        with open(VERSION_FILE, 'r') as f:
            return jsonify(json.load(f))
    return jsonify({"version": "1.0.0"})

@app_flask.route('/manifest_txt')
def get_manifest():
    if os.path.exists(MANIFEST_FILE):
        with open(MANIFEST_FILE, 'r') as f:
            return f.read(), 200, {'Content-Type': 'text/plain'}
    return "manifest_not_found", 404

@app_flask.route('/download/<path:filename>')
def download_file(filename):
    filepath = os.path.join(FILES_DIR, filename)
    if os.path.exists(filepath):
        return send_file(filepath)
    return f"File not found: {filename}", 404

def run_server_thread():
    serve(app_flask, listen='*:5000 [::]:5000')

class ServerAppTk:
    def __init__(self, root):
        self.root = root
        self.root.title("QuickSTT Master Server")
        self.root.geometry("750x500")
        self.root.configure(bg="#1a1a1a")

        title_frame = tk.Frame(self.root, bg="#1a1a1a")
        title_frame.pack(fill=tk.X, padx=20, pady=(20, 10))
        lbl_title = tk.Label(title_frame, text="\U0001F4E1 QuickSTT Server", bg="#1a1a1a", fg="#38bdf8", font=("Segoe UI", 16, "bold"))
        lbl_title.pack(side=tk.LEFT)
        lbl_status = tk.Label(title_frame, text="\u25CF ACTIVE", bg="#064e3b", fg="#4ade80", font=("Segoe UI", 12, "bold"), padx=10, pady=2)
        lbl_status.pack(side=tk.RIGHT)

        lbl_sub = tk.Label(self.root, text=f"Serving from:\n{SERVER_DIR}", bg="#1a1a1a", fg="#94a3b8", font=("Segoe UI", 10), justify=tk.LEFT)
        lbl_sub.pack(anchor=tk.W, padx=20, pady=(0, 10))

        self.log_box = scrolledtext.ScrolledText(self.root, bg="#2b2b2b", fg="#4ade80", font=("Consolas", 10), bd=0, relief=tk.FLAT, padx=10, pady=10)
        self.log_box.pack(fill=tk.BOTH, expand=True, padx=20, pady=(0, 10))
        self.log_box.config(state=tk.DISABLED)

        btn = tk.Button(self.root, text="\U0001F6E1\uFE0F  Allow Through Firewall", bg="#0284c7", fg="white", font=("Segoe UI", 11, "bold"), bd=0, relief=tk.FLAT, padx=15, pady=8, command=self.add_firewall_rules)
        btn.pack(pady=(0, 20))

        self.root.after(100, self.start_server)

    def safe_log(self, text):
        self.log_box.config(state=tk.NORMAL)
        self.log_box.insert(tk.END, f"> {text}\n")
        self.log_box.see(tk.END)
        self.log_box.config(state=tk.DISABLED)

    def start_server(self):
        self.safe_log("Initializing system...")
        self.safe_log(f"Files dir: {FILES_DIR}")
        self.safe_log(f"Manifest: {MANIFEST_FILE} (exists={os.path.exists(MANIFEST_FILE)})")
        self.safe_log("Starting Waitress on *:5000 and [::]:5000...")
        t = threading.Thread(target=run_server_thread, daemon=True)
        t.start()
        self.safe_log("Server listening on IPv4 + IPv6 Port 5000!")
        self.add_firewall_rules()

    def add_firewall_rules(self):
        self.safe_log("Checking firewall...")
        try:
            if ctypes.windll.shell32.IsUserAnAdmin():
                for cmd in [
                    'netsh advfirewall firewall add rule name="QuickSTT_Server_5000" dir=in action=allow protocol=TCP localport=5000',
                    'netsh advfirewall firewall add rule name="QuickSTT_Server_5000_out" dir=out action=allow protocol=TCP localport=5000',
                ]:
                    subprocess.run(cmd, shell=True, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                self.safe_log("SUCCESS: Firewall rules applied.")
            else:
                self.safe_log("WARNING: Not admin. Run as Admin for firewall setup.")
        except Exception as e:
            self.safe_log(f"ERROR: {e}")

class Interceptor:
    def __init__(self, app):
        self.app = app
    def write(self, msg):
        if msg.strip():
            self.app.root.after(0, self.app.safe_log, msg.strip())
    def flush(self):
        pass

def main():
    root = tk.Tk()
    app = ServerAppTk(root)
    sys.stdout = Interceptor(app)
    sys.stderr = Interceptor(app)
    root.mainloop()

if __name__ == '__main__':
    main()
