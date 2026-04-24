#!/usr/bin/env python3
"""
StickS3 Voice Keyboard Helper (Windows tray app)

Runs in the Windows system tray. Responsibilities:

  1. HTTP server on configurable port (default 8765):
     - POST /type    — paste text into focused window (Win32 clipboard + Ctrl+V)
     - POST /status  — receive Claude Code progress from hooks (ring buffer)
     - GET  /status  — return buffer for StickS3 to poll
     - POST /log     — append a line to stick_log.txt (persistent board log)
     - GET  /ping    — liveness probe

  2. UDP discovery responder on configurable port (default 8766):
     - StickS3 broadcasts "STICK?" to locate a helper on the LAN.
     - This helper replies "STICK! <ip>:<http_port>" so the board can cache
       the address without any manual configuration.

  3. System tray icon (pystray):
     - Tooltip shows LAN IP + port + connection status.
     - Right-click menu: "打开配置" / "打开日志文件夹" / "退出".

  4. Config stored in helper/config.json (auto-created with defaults).
     Tkinter config dialog lets the user tweak: port, corrections,
     discovery toggle, open-on-login shortcut.

Install once:
    pip install pystray Pillow

Run (double-click or CLI):
    python type_server.py
"""

import atexit
import ctypes
import json
import os
import re
import socket
import sys
import threading
import time
from collections import deque
from ctypes import wintypes
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn

# ==========================================================================
# Config
# ==========================================================================

# Base directory for persistent files (config + log). Under a PyInstaller
# frozen exe, __file__ points INTO the _MEIxxx temp extraction dir — that
# directory gets wiped when the exe exits, so writing config/log there
# makes both disappear on restart. Use the exe's containing folder instead.
if getattr(sys, "frozen", False):
    _THIS_DIR = os.path.dirname(os.path.abspath(sys.executable))
else:
    _THIS_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(_THIS_DIR, "config.json")
LOG_FILE    = os.path.join(_THIS_DIR, "stick_log.txt")

DEFAULT_CONFIG = {
    "http_port": 8765,
    "udp_port": 8766,
    "discovery_enabled": True,
    "autostart": False,
    "corrections": [
        ["克拉克", "Claude"],
        ["克劳德", "Claude"],
        ["卡劳德", "Claude"],
        ["cloud\\s*code", "Claude Code"],
        ["claude\\s*code", "Claude Code"],
        ["GitHub", "GitHub"],
        ["社会上", "设备上"],
    ],
}


def load_config():
    if not os.path.exists(CONFIG_PATH):
        _save(DEFAULT_CONFIG)
        return dict(DEFAULT_CONFIG)
    try:
        with open(CONFIG_PATH, encoding="utf-8") as f:
            cfg = json.load(f)
    except Exception:
        cfg = {}
    for k, v in DEFAULT_CONFIG.items():
        cfg.setdefault(k, v)
    return cfg


def _save(cfg):
    with open(CONFIG_PATH, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2, ensure_ascii=False)


def save_config(cfg):
    global CONFIG, _compiled_corrections
    _save(cfg)
    CONFIG = cfg
    _compiled_corrections = _compile_corrections(cfg["corrections"])


def _compile_corrections(pairs):
    compiled = []
    for row in pairs:
        if not isinstance(row, (list, tuple)) or len(row) != 2:
            print(f"[warn] skipping malformed correction row: {row!r}")
            continue
        pat, repl = row
        try:
            compiled.append((re.compile(pat, re.IGNORECASE), repl))
        except re.error as e:
            print(f"[warn] skipping invalid regex {pat!r}: {e}")
    return compiled


CONFIG = load_config()
_compiled_corrections = _compile_corrections(CONFIG["corrections"])


def fix_text(text: str) -> str:
    for pat, repl in _compiled_corrections:
        text = pat.sub(repl, text)
    return text


# ==========================================================================
# Win32 clipboard + SendInput (IME-safe paste path)
# ==========================================================================
if sys.platform != "win32":
    print("[error] Windows only."); sys.exit(1)

user32   = ctypes.WinDLL("user32",   use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

CF_UNICODETEXT = 13
GMEM_MOVEABLE  = 0x0002

INPUT_KEYBOARD    = 1
KEYEVENTF_KEYUP   = 0x0002
VK_CONTROL = 0x11
VK_V       = 0x56
VK_RETURN  = 0x0D

ULONG_PTR = wintypes.WPARAM


class MOUSEINPUT(ctypes.Structure):
    _fields_ = (("dx", wintypes.LONG), ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ULONG_PTR))

class KEYBDINPUT(ctypes.Structure):
    _fields_ = (("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ULONG_PTR))

class HARDWAREINPUT(ctypes.Structure):
    _fields_ = (("uMsg", wintypes.DWORD),
                ("wParamL", wintypes.WORD), ("wParamH", wintypes.WORD))

class INPUT(ctypes.Structure):
    class _I(ctypes.Union):
        _fields_ = (("ki", KEYBDINPUT), ("mi", MOUSEINPUT), ("hi", HARDWAREINPUT))
    _anonymous_ = ("i",)
    _fields_ = (("type", wintypes.DWORD), ("i", _I))

LPINPUT = ctypes.POINTER(INPUT)

user32.SendInput.argtypes = (wintypes.UINT, LPINPUT, ctypes.c_int)
user32.SendInput.restype  = wintypes.UINT

kernel32.GlobalAlloc.argtypes  = (wintypes.UINT, ctypes.c_size_t)
kernel32.GlobalAlloc.restype   = wintypes.HANDLE
kernel32.GlobalLock.argtypes   = (wintypes.HANDLE,)
kernel32.GlobalLock.restype    = ctypes.c_void_p
kernel32.GlobalUnlock.argtypes = (wintypes.HANDLE,)
kernel32.GlobalUnlock.restype  = wintypes.BOOL
kernel32.GlobalFree.argtypes   = (wintypes.HANDLE,)
kernel32.GlobalFree.restype    = wintypes.HANDLE

user32.OpenClipboard.argtypes    = (wintypes.HWND,)
user32.OpenClipboard.restype     = wintypes.BOOL
user32.EmptyClipboard.argtypes   = ()
user32.EmptyClipboard.restype    = wintypes.BOOL
user32.SetClipboardData.argtypes = (wintypes.UINT, wintypes.HANDLE)
user32.SetClipboardData.restype  = wintypes.HANDLE
user32.CloseClipboard.argtypes   = ()
user32.CloseClipboard.restype    = wintypes.BOOL


def set_clipboard_text(text: str) -> bool:
    data = text.encode("utf-16le") + b"\x00\x00"
    size = len(data)
    for _ in range(10):
        if user32.OpenClipboard(0): break
        time.sleep(0.05)
    else:
        return False
    hmem = None
    try:
        user32.EmptyClipboard()
        hmem = kernel32.GlobalAlloc(GMEM_MOVEABLE, size)
        if not hmem: return False
        ptr = kernel32.GlobalLock(hmem)
        if not ptr: return False
        ctypes.memmove(ptr, data, size)
        kernel32.GlobalUnlock(hmem)
        if not user32.SetClipboardData(CF_UNICODETEXT, hmem):
            return False
        # On success the system owns hmem — clear our handle so the
        # finally block doesn't GlobalFree it out from under the clipboard.
        hmem = None
        return True
    finally:
        if hmem:
            kernel32.GlobalFree(hmem)
        user32.CloseClipboard()


def _vk_event(vk, up=False):
    flags = KEYEVENTF_KEYUP if up else 0
    return INPUT(type=INPUT_KEYBOARD,
                 i=INPUT._I(ki=KEYBDINPUT(vk, 0, flags, 0, 0)))

def _send(events):
    arr = (INPUT * len(events))(*events)
    user32.SendInput(len(events), arr, ctypes.sizeof(INPUT))

def _release_all_modifiers():
    for vk in (VK_CONTROL, 0xA2, 0xA3, 0x10, 0xA0, 0xA1, 0x12, 0xA4, 0xA5, 0x5B, 0x5C):
        try: _send([_vk_event(vk, up=True)])
        except Exception: pass

atexit.register(_release_all_modifiers)


def send_ctrl_v():
    try:
        _send([_vk_event(VK_CONTROL),
               _vk_event(VK_V),
               _vk_event(VK_V, up=True),
               _vk_event(VK_CONTROL, up=True)])
    except BaseException:
        _release_all_modifiers()
        raise

def send_enter():
    _send([_vk_event(VK_RETURN), _vk_event(VK_RETURN, up=True)])


def paste_and_enter(text: str, press_enter: bool = True) -> bool:
    if not set_clipboard_text(text): return False
    time.sleep(0.06)
    send_ctrl_v()
    if press_enter:
        time.sleep(0.20)
        send_enter()
    return True


# ==========================================================================
# Claude status ring buffer
# ==========================================================================
_status_lock = threading.Lock()
# Protects concurrent appends to LOG_FILE. Python's GIL makes short
# writes atomic-ish but the two /status_event + /log handlers serve from
# different worker threads and long multi-line entries can interleave
# without a lock.
_log_file_lock = threading.Lock()
_status_log = deque(maxlen=16)
_seq = 0


def _clean_text(s: str) -> str:
    if not s:
        return s
    try:
        return s.encode("utf-8", errors="replace").decode("utf-8", errors="replace")
    except Exception:
        return s.encode("utf-8", errors="ignore").decode("utf-8", errors="ignore")


def add_status(text: str) -> None:
    global _seq
    text = _clean_text((text or "").strip())
    if not text:
        return
    with _status_lock:
        _seq += 1
        _status_log.append({"text": text, "ts": time.time(), "seq": _seq})
    try:
        with _log_file_lock, open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] [claude] {text}\n")
    except Exception as e:
        print(f"[warn] status→log write failed: {e}")


def get_status_snapshot() -> dict:
    now = time.time()
    with _status_lock:
        items = list(_status_log)
    latest = items[-1] if items else None
    return {
        "latest":     (latest.get("text", "") if latest else ""),
        "latest_seq": (latest.get("seq", 0) if latest else 0),
        "age_sec":    (int(now - latest.get("ts", now)) if latest else -1),
        "history":    [{"text": it.get("text", ""), "seq": it.get("seq", 0)}
                       for it in items[-8:]],
        "now":        int(now),
    }


# ==========================================================================
# HTTP server
# ==========================================================================
class Handler(BaseHTTPRequestHandler):
    def _send_json(self, code, obj):
        try:
            body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        except Exception as e:
            print(f"[warn] json encode failed: {e}")
            body = b'{"error":"encode"}'
            code = 500
        try:
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
        except Exception as e:
            print(f"[warn] send failed ({self.path}): {e}")

    def do_GET(self):
        if self.path == "/ping":
            self._send_json(200, {"ok": True, "service": "sticks3-typebuddy"})
        elif self.path == "/status":
            self._send_json(200, get_status_snapshot())
        else:
            self._send_json(404, {"error": "unknown path"})

    def do_POST(self):
        try:
            length = int(self.headers.get("Content-Length", 0) or 0)
        except (TypeError, ValueError):
            self._send_json(400, {"error": "bad content-length"})
            return
        if length < 0 or length > 1_048_576:  # hard cap at 1 MB (localhost only)
            self._send_json(413, {"error": "body too large"})
            return
        try:
            data = json.loads(self.rfile.read(length) or b"{}")
        except Exception:
            self._send_json(400, {"error": "invalid json"})
            return

        if self.path == "/status":
            add_status(data.get("text", ""))
            self._send_json(200, {"ok": True})
            return

        if self.path == "/log":
            line = (data.get("text") or "").strip()
            level = (data.get("level") or "info").strip()
            src_ip = self.client_address[0] if self.client_address else "?"
            if line:
                try:
                    with _log_file_lock, open(LOG_FILE, "a", encoding="utf-8") as f:
                        f.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] "
                                f"[{level}] [from {src_ip}] {line}\n")
                except Exception as e:
                    print(f"[warn] log write failed: {e}")
            self._send_json(200, {"ok": True})
            return

        if self.path != "/type":
            self._send_json(404, {"error": "unknown path"})
            return

        text = (data.get("text") or "").strip()
        press_enter = bool(data.get("enter", True))
        if not text:
            self._send_json(400, {"error": "no text"})
            return

        fixed = fix_text(text)
        try:
            ok = paste_and_enter(fixed, press_enter)
        except Exception as e:
            print(f"[error] paste failed: {e}")
            self._send_json(500, {"error": str(e)})
            return

        if ok:
            if fixed != text:
                print(f"[typed] {fixed}   (was: {text})")
            else:
                print(f"[typed] {fixed}")
        else:
            print(f"[warn] paste_and_enter returned False for: {fixed}")

        self._send_json(200, {"ok": bool(ok), "chars": len(fixed)})

    def log_message(self, *args, **kwargs):
        pass


class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True


# ==========================================================================
# UDP discovery responder
#   Board broadcasts b"STICK?" to udp_port → reply b"STICK! <ip>:<http_port>"
# ==========================================================================
def discovery_loop():
    if not CONFIG.get("discovery_enabled", True):
        return
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(("0.0.0.0", CONFIG["udp_port"]))
    except Exception as e:
        print(f"[warn] UDP discovery bind failed: {e}")
        return
    print(f"[discovery] listening on UDP :{CONFIG['udp_port']}")
    while True:
        try:
            data, addr = s.recvfrom(512)
        except Exception:
            continue
        if not data:
            continue
        if data.startswith(b"STICK?"):
            ip = local_ip()
            reply = f"STICK! {ip}:{CONFIG['http_port']}".encode("ascii")
            try:
                s.sendto(reply, addr)
                print(f"[discovery] replied to {addr[0]} → {ip}:{CONFIG['http_port']}")
            except Exception as e:
                print(f"[warn] discovery reply failed: {e}")


def local_ip() -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


# ==========================================================================
# Autostart (Startup folder shortcut)
# ==========================================================================
STARTUP_SHORTCUT = os.path.join(
    os.environ.get("APPDATA", ""),
    r"Microsoft\Windows\Start Menu\Programs\Startup\StickS3 Helper.lnk",
)


def set_autostart(enable: bool) -> None:
    if enable:
        try:
            # Pick the right target based on whether we're running as a
            # PyInstaller-frozen exe or as a plain .py script. Under the
            # exe, __file__ points at a _MEIxxx temp dir that vanishes
            # when the process exits — the shortcut has to point at
            # sys.executable (the exe itself) instead.
            if getattr(sys, "frozen", False):
                target = sys.executable
                args = ""
                workdir = os.path.dirname(sys.executable)
            else:
                target = "pythonw.exe"
                args = f'"{os.path.abspath(__file__)}"'
                workdir = _THIS_DIR
            ps = (
                f'$s=(New-Object -ComObject WScript.Shell).CreateShortcut('
                f'"{STARTUP_SHORTCUT}");'
                f'$s.TargetPath="{target}";'
            )
            if args:
                ps += f'$s.Arguments=\'{args}\';'
            ps += f'$s.WorkingDirectory="{workdir}";$s.Save()'
            import subprocess
            subprocess.run(["powershell", "-Command", ps], check=False,
                           creationflags=0x08000000)  # CREATE_NO_WINDOW
        except Exception as e:
            print(f"[warn] autostart enable failed: {e}")
    else:
        try:
            if os.path.exists(STARTUP_SHORTCUT):
                os.remove(STARTUP_SHORTCUT)
        except Exception as e:
            print(f"[warn] autostart disable failed: {e}")


# ==========================================================================
# Config dialog (tkinter)
# ==========================================================================
def open_config_dialog():
    try:
        import tkinter as tk
        from tkinter import ttk, messagebox
    except Exception as e:
        print(f"[warn] tkinter missing: {e}")
        return

    root = tk.Tk()
    root.title("StickS3 助手 · 配置")
    root.geometry("480x520")
    root.resizable(False, False)

    frm = ttk.Frame(root, padding=12)
    frm.pack(fill="both", expand=True)

    # IP/port info (read-only)
    ttk.Label(frm, text=f"本机 IP: {local_ip()}", foreground="gray").pack(anchor="w")

    row = ttk.Frame(frm); row.pack(fill="x", pady=6)
    ttk.Label(row, text="HTTP 端口").grid(row=0, column=0, sticky="w")
    http_var = tk.StringVar(value=str(CONFIG["http_port"]))
    ttk.Entry(row, textvariable=http_var, width=10).grid(row=0, column=1, padx=6)
    ttk.Label(row, text="UDP 发现端口").grid(row=0, column=2, sticky="w", padx=(12, 0))
    udp_var = tk.StringVar(value=str(CONFIG["udp_port"]))
    ttk.Entry(row, textvariable=udp_var, width=10).grid(row=0, column=3, padx=6)

    discovery_var = tk.BooleanVar(value=CONFIG["discovery_enabled"])
    ttk.Checkbutton(frm, text="启用 UDP 自动发现", variable=discovery_var).pack(anchor="w", pady=4)

    autostart_var = tk.BooleanVar(value=CONFIG.get("autostart", False))
    ttk.Checkbutton(frm, text="开机自动启动", variable=autostart_var).pack(anchor="w", pady=4)

    ttk.Label(frm, text="语音纠错词表（每行一对，格式 `正则=替换`）:").pack(anchor="w", pady=(8, 2))
    txt = tk.Text(frm, height=12, font=("Consolas", 10))
    txt.pack(fill="both", expand=True)
    for pat, repl in CONFIG["corrections"]:
        txt.insert("end", f"{pat}={repl}\n")

    def on_save():
        try:
            http_port = int(http_var.get())
            udp_port = int(udp_var.get())
        except ValueError:
            messagebox.showerror("错误", "端口必须是数字")
            return
        corr = []
        for line in txt.get("1.0", "end").splitlines():
            line = line.strip()
            if not line or "=" not in line:
                continue
            pat, _, repl = line.partition("=")
            corr.append([pat.strip(), repl.strip()])
        new_cfg = {
            "http_port": http_port,
            "udp_port": udp_port,
            "discovery_enabled": discovery_var.get(),
            "autostart": autostart_var.get(),
            "corrections": corr,
        }
        save_config(new_cfg)
        set_autostart(autostart_var.get())
        messagebox.showinfo("已保存",
                            "修改已保存到 config.json。\n\n"
                            "端口改动需要重启助手才生效。")
        root.destroy()

    btn_row = ttk.Frame(frm); btn_row.pack(fill="x", pady=(10, 0))
    ttk.Button(btn_row, text="保存", command=on_save).pack(side="right")
    ttk.Button(btn_row, text="取消", command=root.destroy).pack(side="right", padx=8)

    root.mainloop()


# ==========================================================================
# Tray icon
# ==========================================================================
def open_log_folder(icon=None, item=None):
    try:
        os.startfile(_THIS_DIR)
    except Exception as e:
        print(f"[warn] open log folder: {e}")


def on_quit(icon, item=None):
    # os._exit bypasses atexit handlers, so the Ctrl/Shift/etc release
    # pass we registered there won't fire. Call it explicitly so no
    # modifier key stays system-wide "down" if the user quits mid-paste.
    try:
        _release_all_modifiers()
    except Exception:
        pass
    icon.stop()
    os._exit(0)


def make_icon_image():
    from PIL import Image, ImageDraw
    size = 64
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # Warm orange "sparkle" (Claude style)
    c = (232, 151, 111, 255)
    # four petals
    d.polygon([(32, 4), (36, 32), (32, 32)], fill=c)
    d.polygon([(32, 60), (28, 32), (32, 32)], fill=c)
    d.polygon([(4, 32), (32, 28), (32, 32)], fill=c)
    d.polygon([(60, 32), (32, 36), (32, 32)], fill=c)
    # diagonals (shorter)
    d.polygon([(14, 14), (32, 30), (30, 32)], fill=c)
    d.polygon([(50, 50), (32, 34), (34, 32)], fill=c)
    d.polygon([(14, 50), (30, 32), (32, 34)], fill=c)
    d.polygon([(50, 14), (34, 32), (32, 30)], fill=c)
    # center dot
    d.ellipse((28, 28, 36, 36), fill=c)
    return img


def start_tray():
    try:
        import pystray
    except ImportError:
        print("[error] pystray not installed. Run:  pip install pystray Pillow")
        # Fallback: just block
        while True:
            time.sleep(3600)
        return

    ip = local_ip()
    title = f"StickS3 助手 — {ip}:{CONFIG['http_port']}"

    menu = pystray.Menu(
        pystray.MenuItem(f"IP: {ip}:{CONFIG['http_port']}", None, enabled=False),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem("打开配置", lambda icon, item: threading.Thread(
            target=open_config_dialog, daemon=True).start()),
        pystray.MenuItem("打开日志文件夹", open_log_folder),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem("退出", on_quit),
    )
    icon = pystray.Icon("StickS3Helper", make_icon_image(), title, menu)
    icon.run()


# ==========================================================================
# main
# ==========================================================================
def main():
    ip = local_ip()
    print("=" * 60)
    print(f"  StickS3 Voice Keyboard Helper")
    print(f"  HTTP :{CONFIG['http_port']}   UDP discovery :{CONFIG['udp_port']}")
    print(f"  LAN IP: {ip}")
    print(f"  Log:    {LOG_FILE}")
    print(f"  Config: {CONFIG_PATH}")
    print("=" * 60)

    # HTTP server thread
    try:
        http_srv = ThreadedHTTPServer(("0.0.0.0", CONFIG["http_port"]), Handler)
    except OSError as e:
        print(f"[error] HTTP port {CONFIG['http_port']} in use: {e}")
        sys.exit(1)
    threading.Thread(target=http_srv.serve_forever, daemon=True).start()

    # UDP discovery thread
    threading.Thread(target=discovery_loop, daemon=True).start()

    # Tray icon runs on main thread (required by pystray)
    try:
        start_tray()
    except KeyboardInterrupt:
        print("\n[stopped]")


if __name__ == "__main__":
    main()
