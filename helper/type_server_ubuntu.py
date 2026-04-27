#!/usr/bin/env python3
"""
StickS3 Claude Codex Helper — Ubuntu/Linux Desktop Adaptation

基于 type_server.py 修改，适配 Linux 桌面环境：
- 替换 Win32 API 为 xdotool + pyperclip
- 替换 Windows 系统托盘为 Linux notify + pystray
- 替换 Windows 单例机制为文件锁
- 替换 Windows 自启为 XDG autostart

依赖: pystray Pillow pyperclip python-xlib xdotool
"""

import fcntl
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
import zipfile
from collections import deque
from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path
from socketserver import ThreadingMixIn

# 复用共享模块
from status_logic import clean_text, codex_phase_for_text
from version_info import HELPER_VERSION, RELEASE_PAGE_URL

# ==========================================================================
# 单例保护（文件锁）
# ==========================================================================
_SINGLE_INSTANCE_LOCK = None
try:
    _LOCK_PATH = os.path.expanduser("~/.config/sticks3-helper.lock")
    os.makedirs(os.path.dirname(_LOCK_PATH), exist_ok=True)
    _SINGLE_INSTANCE_LOCK = open(_LOCK_PATH, "w")
    fcntl.flock(_SINGLE_INSTANCE_LOCK.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
except (IOError, OSError):
    print("[warn] 另一个助手实例已在运行，或锁文件不可用")
    _SINGLE_INSTANCE_LOCK = None

# ==========================================================================
# Config
# ==========================================================================
_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(_THIS_DIR, "config_ubuntu.json")
LOG_FILE = os.path.join(_THIS_DIR, "stick_log_ubuntu.txt")
CONFIG_SCHEMA_VERSION = 2


def _resource_path(filename: str) -> str:
    """兼容源码运行与 PyInstaller onefile 运行时资源路径。"""
    if getattr(sys, "frozen", False):
        meipass = getattr(sys, "_MEIPASS", None)
        if meipass:
            bundled = os.path.join(meipass, filename)
            if os.path.exists(bundled):
                return bundled
        exe_dir = os.path.dirname(os.path.abspath(sys.executable))
        beside_exe = os.path.join(exe_dir, filename)
        if os.path.exists(beside_exe):
            return beside_exe
    return os.path.join(_THIS_DIR, filename)

DEFAULT_CONFIG = {
    "config_version": CONFIG_SCHEMA_VERSION,
    "http_port": 8765,
    "udp_port": 8766,
    "discovery_enabled": True,
    "autostart": False,
    "codex_session_watch_enabled": False,
    "target_window": None,
    "codex_target_window": None,
    "target_click": None,
    "codex_target_click": None,
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


def _fresh_default_config():
    return json.loads(json.dumps(DEFAULT_CONFIG, ensure_ascii=False))


def load_config():
    if not os.path.exists(CONFIG_PATH):
        cfg = _fresh_default_config()
        _save(cfg)
        return cfg
    try:
        with open(CONFIG_PATH, encoding="utf-8") as f:
            cfg = json.load(f)
    except Exception as e:
        backup = CONFIG_PATH + f".bad-{int(time.time())}"
        try:
            os.replace(CONFIG_PATH, backup)
            print(f"[warn] config unreadable, backed up to {backup}: {e}")
        except Exception:
            print(f"[warn] config unreadable and backup failed: {e}")
        cfg = _fresh_default_config()
        _save(cfg)
        return cfg
    changed = False
    for k, v in DEFAULT_CONFIG.items():
        if k not in cfg:
            cfg[k] = v
            changed = True
    for old_key in ("helper_release_api_url", "firmware_manifest_url"):
        if old_key in cfg:
            cfg.pop(old_key, None)
            changed = True
    if cfg.get("config_version") != CONFIG_SCHEMA_VERSION:
        cfg["config_version"] = CONFIG_SCHEMA_VERSION
        changed = True
    if changed:
        _save(cfg)
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
# Linux: 剪贴板 (pyperclip) + xdotool 键盘模拟
# ==========================================================================

def _run(args, timeout=5):
    """安全执行外部命令"""
    try:
        return subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    except Exception as e:
        print(f"[warn] xdotool failed: {e}")
        return None


def set_clipboard_text(text: str) -> bool:
    """设置剪贴板内容"""
    try:
        import pyperclip
        pyperclip.copy(text)
        return True
    except Exception as e:
        print(f"[error] clipboard set failed: {e}")
        return False


def send_ctrl_v():
    """Ctrl+V 粘贴"""
    _run(["xdotool", "key", "ctrl+v"])
    time.sleep(0.05)


def send_enter():
    """回车"""
    _run(["xdotool", "key", "Return"])
    time.sleep(0.05)


def paste_and_enter(text: str, press_enter: bool = True, target_key="target_window") -> bool:
    """粘贴文本并在需要时按回车"""
    # 如果配置了目标窗口，尝试激活
    tgt = CONFIG.get(target_key)
    if tgt and tgt.get("wid"):
        _run(["xdotool", "windowactivate", "--sync", tgt["wid"]])
        time.sleep(0.12)

        # 如果配置了点击位置，先点击该位置
        click_key = "codex_target_click" if target_key == "codex_target_window" else "target_click"
        click_info = CONFIG.get(click_key)
        if click_info and tgt.get("wid"):
            _click_at_window(tgt["wid"], click_info)
            time.sleep(0.08)

    if not set_clipboard_text(text):
        return False
    time.sleep(0.06)
    send_ctrl_v()
    if press_enter:
        time.sleep(0.20)
        send_enter()
    return True


def _click_at_window(wid, click_info):
    """在窗口内指定位置点击"""
    # click_info 包含 x, y（相对于窗口左上角）
    local_x = click_info.get("x", 0)
    local_y = click_info.get("y", 0)
    _run(["xdotool", "mousemove", "--window", wid, str(local_x), str(local_y)])
    time.sleep(0.04)
    _run(["xdotool", "click", "1"])
    time.sleep(0.03)


def capture_current_window():
    """获取当前前台窗口信息，返回 {wid, title, class}"""
    result = _run(["xdotool", "getactivewindow", "getwindowname"])
    if not result or result.returncode != 0:
        return None
    title = result.stdout.strip()
    result2 = _run(["xdotool", "getactivewindow", "getwindowpid"])
    pid = result2.stdout.strip() if result2 and result2.returncode == 0 else None
    result3 = _run(["xdotool", "getactivewindow"])
    wid = result3.stdout.strip() if result3 and result3.returncode == 0 else None
    if not wid:
        return None
    # 获取窗口 class（使用 xprop）
    class_name = ""
    if wid:
        result4 = _run(["xprop", "-id", wid, "WM_CLASS"])
        if result4 and result4.returncode == 0:
            match = re.search(r'WM_CLASS.*"([^"]+)"', result4.stdout)
            if match:
                class_name = match.group(1)
    info = {"wid": wid, "title": title, "class": class_name}
    parts = [p.strip() for p in title.split(" - ") if p.strip()]
    if len(parts) >= 2:
        info["title_tail"] = " - ".join(parts[-2:])
        info["project"] = parts[-2]
    return info


def find_target_window(config_key="target_window"):
    """根据保存的信息找到目标窗口"""
    tgt = CONFIG.get(config_key)
    if not tgt or not tgt.get("wid"):
        return None
    wid = tgt["wid"]
    # 验证窗口是否仍然存在
    result = _run(["xdotool", "windowactivate", wid])
    if result and result.returncode == 0:
        return wid
    return None


def focus_window(wid):
    """激活指定窗口"""
    _run(["xdotool", "windowactivate", "--sync", wid])
    time.sleep(0.1)
    return True


def capture_click_position(config_key="target_window"):
    """捕获鼠标在目标窗口中的相对位置"""
    tgt = CONFIG.get(config_key)
    wid = None
    if tgt and tgt.get("wid"):
        wid = tgt["wid"]
    if not wid:
        result = _run(["xdotool", "getactivewindow"])
        wid = result.stdout.strip() if result and result.returncode == 0 else None
    if not wid:
        return None

    # 获取窗口几何
    result = _run(["xdotool", "getwindowgeometry", wid])
    if not result or result.returncode != 0:
        return None

    # 获取鼠标位置
    result2 = _run(["xdotool", "getmouselocation"])
    if not result2 or result2.returncode != 0:
        return None
    # 输出格式: x:123 y:456 screen:0 window:789
    match = re.search(r'x:(\d+) y:(\d+)', result2.stdout)
    if not match:
        return None
    mouse_x = int(match.group(1))
    mouse_y = int(match.group(2))

    # 获取窗口位置
    geom_match = re.search(r'Position: (\d+),(\d+)', result.stdout)
    if not geom_match:
        return None
    win_x = int(geom_match.group(1))
    win_y = int(geom_match.group(2))

    # 解析尺寸
    size_match = re.search(r'Geometry: (\d+)x(\d+)', result.stdout)
    if not size_match:
        return None
    w = int(size_match.group(1))
    h = int(size_match.group(2))

    local_x = mouse_x - win_x
    local_y = mouse_y - win_y
    return {
        "x": local_x,
        "y": local_y,
        "w": w,
        "h": h,
        "rx": local_x / w if w > 0 else 0,
        "ry": local_y / h if h > 0 else 0,
        "right": w - local_x,
        "bottom": h - local_y,
    }


# ==========================================================================
# Claude status ring buffer（复用原版逻辑）
# ==========================================================================
_status_lock = threading.Lock()
_log_file_lock = threading.Lock()
_status_log = deque(maxlen=16)
_seq = 0
_codex_status_log = deque(maxlen=16)
_codex_seq = 0
_codex_phase = "idle"


def _status_state(channel: str):
    if channel == "codex":
        return _codex_status_log, "_codex_seq", "codex"
    return _status_log, "_seq", "claude"


def add_status(text: str, channel: str = "claude") -> int | None:
    global _seq, _codex_seq, _codex_phase
    text = clean_text((text or "").strip())
    if not text:
        return None
    log, _, label = _status_state(channel)
    with _status_lock:
        if log:
            last = log[-1]
            if last.get("text") == text and time.time() - float(last.get("ts", 0)) < 3.0:
                return int(last.get("seq", 0))
        if channel == "codex":
            _codex_seq += 1
            seq = _codex_seq
        else:
            _seq += 1
            seq = _seq
        log.append({"text": text, "ts": time.time(), "seq": seq})
        if channel == "codex":
            _codex_phase = codex_phase_for_text(text)
    try:
        with _log_file_lock, open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] [{label}] {text}\n")
    except Exception as e:
        print(f"[warn] status→log write failed: {e}")
    return seq


def get_status_snapshot(channel: str = "claude") -> dict:
    now = time.time()
    log, _, _ = _status_state(channel)
    with _status_lock:
        items = list(log)
        phase = _codex_phase if channel == "codex" else "idle"
    latest = items[-1] if items else None
    return {
        "latest":     (latest.get("text", "") if latest else ""),
        "latest_seq": (latest.get("seq", 0) if latest else 0),
        "age_sec":    (int(now - latest.get("ts", now)) if latest else -1),
        "history":    [{"text": it.get("text", ""), "seq": it.get("seq", 0)}
                       for it in items[-8:]],
        "state":      phase,
        "now":        int(now),
    }


# ==========================================================================
# HTTP server（不变）
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
            self._send_json(200, {
                "ok": True,
                "service": "sticks3-helper-ubuntu",
                "helper_version": HELPER_VERSION,
                "config_version": CONFIG.get("config_version"),
            })
        elif self.path == "/status":
            self._send_json(200, get_status_snapshot())
        elif self.path == "/codex/status":
            self._send_json(200, get_status_snapshot("codex"))
        else:
            self._send_json(404, {"error": "unknown path"})

    def do_POST(self):
        try:
            length = int(self.headers.get("Content-Length", 0) or 0)
        except (TypeError, ValueError):
            self._send_json(400, {"error": "bad content-length"})
            return
        if length < 0 or length > 1_048_576:
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

        if self.path == "/codex/status":
            add_status(data.get("text", ""), "codex")
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

        if self.path not in ("/type", "/codex/type"):
            self._send_json(404, {"error": "unknown path"})
            return

        text = (data.get("text") or "").strip()
        press_enter = bool(data.get("enter", True))
        if not text:
            self._send_json(400, {"error": "no text"})
            return

        is_codex = self.path == "/codex/type"
        fixed = fix_text(text)
        try:
            ok = paste_and_enter(
                fixed, press_enter,
                "codex_target_window" if is_codex else "target_window"
            )
        except Exception as e:
            print(f"[error] paste failed: {e}")
            self._send_json(500, {"error": str(e)})
            return

        if ok:
            channel = "codex" if is_codex else "claude"
            add_status("\x01U" + fixed, channel)
            if fixed != text:
                print(f"[typed:{'codex' if is_codex else 'claude'}] {fixed}   (was: {text})")
            else:
                print(f"[typed:{'codex' if is_codex else 'claude'}] {fixed}")
        else:
            print(f"[warn] paste_and_enter returned False for: {fixed}")

        self._send_json(200, {"ok": bool(ok), "chars": len(fixed)})

    def log_message(self, *args, **kwargs):
        pass


class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True


# ==========================================================================
# UDP discovery responder（不变）
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
# Linux 自启（XDG autostart）
# ==========================================================================
AUTOSTART_DESKTOP = os.path.expanduser("~/.config/autostart/sticks3-helper.desktop")


def set_autostart(enable: bool) -> None:
    if enable:
        if getattr(sys, "frozen", False):
            exec_cmd = os.path.abspath(sys.executable)
            icon_path = _resource_path("icon_64.png")
        else:
            exec_cmd = f"{sys.executable} {os.path.abspath(__file__)}"
            icon_path = os.path.join(_THIS_DIR, "icon_64.png")
        lines = [
            "[Desktop Entry]",
            "Type=Application",
            "Name=StickS3 Helper",
            f"Exec={exec_cmd}",
            f"Icon={icon_path}",
            "Terminal=false",
            "Hidden=false",
            "X-GNOME-Autostart-enabled=true",
        ]
        try:
            os.makedirs(os.path.dirname(AUTOSTART_DESKTOP), exist_ok=True)
            with open(AUTOSTART_DESKTOP, "w") as f:
                f.write("\n".join(lines) + "\n")
            print(f"[autostart] enabled: {AUTOSTART_DESKTOP}")
        except Exception as e:
            print(f"[warn] autostart enable failed: {e}")
    else:
        try:
            if os.path.exists(AUTOSTART_DESKTOP):
                os.remove(AUTOSTART_DESKTOP)
                print(f"[autostart] disabled")
        except Exception as e:
            print(f"[warn] autostart disable failed: {e}")


# ==========================================================================
# 系统托盘（AppIndicator3 + notify-send）
# ==========================================================================

def _notify(title: str, message: str):
    """发送 Linux 桌面通知"""
    try:
        subprocess.run(
            ["notify-send", "-a", "StickS3 Helper", title, message],
            capture_output=True, timeout=3
        )
    except Exception as e:
        print(f"[warn] notify-send failed: {e}")


def start_tray():
    """使用 GTK AppIndicator3 创建系统托盘"""
    try:
        import gi
        gi.require_version('Gtk', '3.0')
        gi.require_version('AppIndicator3', '0.1')
        from gi.repository import Gtk, AppIndicator3
    except Exception as e:
        print(f"[error] GTK/AppIndicator3 not available: {e}")
        print("Install: sudo apt install python3-gi gir1.2-appindicator3-0.1")
        return

    ip = local_ip()
    tray_icon_path = _resource_path("icon_64.png")

    class TrayIcon:
        def __init__(self):
            self.indicator = AppIndicator3.Indicator.new(
                'sticks3-helper',
                'application-default-icon',
                AppIndicator3.IndicatorCategory.APPLICATION_STATUS
            )
            if os.path.exists(tray_icon_path):
                self.indicator.set_icon_full(tray_icon_path, "StickS3 Helper")
            self.indicator.set_status(AppIndicator3.IndicatorStatus.ACTIVE)
            self.indicator.set_title(f"StickS3 {ip}:{CONFIG['http_port']}")
            self.build_menu()

        def format_target(self, config_key="target_window", label="Claude"):
            tgt = CONFIG.get(config_key)
            if not tgt:
                return f"{label}: unbound"
            t = (tgt.get("title") or "").strip() or "(no title)"
            if len(t) > 28:
                t = t[:28] + "..."
            return f"{label}: {t}"

        def build_menu(self):
            menu = Gtk.Menu()

            # IP info (disabled item)
            lbl = Gtk.Label(label=f"IP: {ip}:{CONFIG['http_port']}")
            lbl.set_sensitive(False)
            item = Gtk.MenuItem()
            item.add(lbl)
            menu.append(item)

            # Status items (disabled)
            for lbl_txt in [self.format_target("target_window", "Claude"),
                             self.format_target("codex_target_window", "Codex")]:
                lbl = Gtk.Label(label=lbl_txt)
                lbl.set_sensitive(False)
                item = Gtk.MenuItem()
                item.add(lbl)
                menu.append(item)

            menu.append(Gtk.SeparatorMenuItem())

            # Bind window items
            item = Gtk.MenuItem(label="Bind Claude Window (3s)")
            item.connect('activate', lambda _: self.on_bind_target("target_window", "Claude"))
            menu.append(item)

            item = Gtk.MenuItem(label="Bind Codex Window (3s)")
            item.connect('activate', lambda _: self.on_bind_target("codex_target_window", "Codex"))
            menu.append(item)

            item = Gtk.MenuItem(label="Bind Claude Input (3s)")
            item.connect('activate', lambda _: self.on_bind_click("target_window", "target_click", "Claude"))
            menu.append(item)

            item = Gtk.MenuItem(label="Bind Codex Input (3s)")
            item.connect('activate', lambda _: self.on_bind_click("codex_target_window", "codex_target_click", "Codex"))
            menu.append(item)

            # Clear items
            item = Gtk.MenuItem(label="Clear Claude Bind")
            item.connect('activate', lambda _: self.on_clear_target("target_window", "Claude"))
            menu.append(item)

            item = Gtk.MenuItem(label="Clear Codex Bind")
            item.connect('activate', lambda _: self.on_clear_target("codex_target_window", "Codex"))
            menu.append(item)

            menu.append(Gtk.SeparatorMenuItem())

            # Config
            item = Gtk.MenuItem(label="Settings")
            item.connect('activate', lambda _: self.on_open_config())
            menu.append(item)

            autostart_item = Gtk.CheckMenuItem(label="Auto Start On Login")
            autostart_item.set_active(bool(CONFIG.get("autostart", False)))
            autostart_item.connect('toggled', self.on_toggle_autostart)
            menu.append(autostart_item)

            menu.append(Gtk.SeparatorMenuItem())

            # Quit
            item = Gtk.MenuItem(label="Quit")
            item.connect('activate', lambda _: Gtk.main_quit())
            menu.append(item)

            menu.show_all()
            self.indicator.set_menu(menu)

        def on_bind_target(self, config_key, label):
            _notify(f"Bind {label}", "3s to focus target window...")
            time.sleep(3)
            info = capture_current_window()
            if not info:
                _notify(f"{label} bind failed", "no foreground window")
                return
            CONFIG[config_key] = info
            save_config(CONFIG)
            t = (info.get("title") or "").strip() or "no title"
            if len(t) > 36:
                t = t[:36] + "..."
            _notify(f"{label} bound", f"target: {t}")
            print(f"[bind:{label}] locked: {info}")

        def on_bind_click(self, config_key, click_key, label):
            _notify(f"Bind {label} input", "3s to capture position...")
            time.sleep(3)
            pos = capture_click_position(config_key)
            if not pos:
                _notify(f"{label} pos failed", "no position captured")
                return
            CONFIG[click_key] = pos
            save_config(CONFIG)
            _notify(f"{label} pos set", f"input: x={pos['x']} y={pos['y']}")
            print(f"[bind:{label}] click pos: {pos}")

        def on_clear_target(self, config_key, label):
            CONFIG[config_key] = None
            save_config(CONFIG)
            _notify(f"{label} cleared", "target unbound")

        def on_open_config(self):
            import tkinter as tk
            from tkinter import ttk, messagebox

            root = tk.Tk()
            root.title("StickS3 Helper - Settings")
            root.geometry("600x450")

            frm = ttk.Frame(root, padding=12)
            frm.pack(fill="both", expand=True)

            ttk.Label(frm, text="StickS3 Helper (Ubuntu)", font=("Sans", 14, "bold")).pack()
            ttk.Label(frm, text=f"Version {HELPER_VERSION}", foreground="#666").pack()

            status_box = ttk.LabelFrame(frm, text="Status", padding=10)
            status_box.pack(fill="x", pady=10)
            ttk.Label(status_box, text="IP").grid(row=0, column=0, sticky="w")
            ttk.Label(status_box, text=f"{local_ip()}:{CONFIG['http_port']}").grid(row=0, column=1, sticky="w", padx=10)
            ttk.Label(status_box, text="Claude").grid(row=1, column=0, sticky="w", pady=4)
            ttk.Label(status_box, text=self.format_target("target_window", "Claude")).grid(row=1, column=1, sticky="w", padx=10)
            ttk.Label(status_box, text="Codex").grid(row=2, column=0, sticky="w", pady=4)
            ttk.Label(status_box, text=self.format_target("codex_target_window", "Codex")).grid(row=2, column=1, sticky="w", padx=10)

            net_box = ttk.LabelFrame(frm, text="Network", padding=10)
            net_box.pack(fill="x", pady=10)
            http_var = tk.StringVar(value=str(CONFIG["http_port"]))
            udp_var = tk.StringVar(value=str(CONFIG["udp_port"]))
            ttk.Label(net_box, text="HTTP Port").grid(row=0, column=0, sticky="w")
            ttk.Entry(net_box, textvariable=http_var, width=8).grid(row=0, column=1, padx=(8, 20))
            ttk.Label(net_box, text="UDP Port").grid(row=0, column=2, sticky="w")
            ttk.Entry(net_box, textvariable=udp_var, width=8).grid(row=0, column=3, padx=(8, 0))

            def on_save():
                try:
                    CONFIG["http_port"] = int(http_var.get())
                    CONFIG["udp_port"] = int(udp_var.get())
                except ValueError:
                    messagebox.showerror("Error", "Port must be a number")
                    return
                save_config(CONFIG)
                messagebox.showinfo("Saved", "Restart helper for port changes.")
                root.destroy()

            btn_row = ttk.Frame(frm)
            btn_row.pack(pady=10)
            ttk.Button(btn_row, text="Save", command=on_save).pack(side="right")
            ttk.Button(btn_row, text="Cancel", command=root.destroy).pack(side="right", padx=8)

            root.mainloop()

        def on_toggle_autostart(self, item):
            enabled = bool(item.get_active())
            CONFIG["autostart"] = enabled
            save_config(CONFIG)
            set_autostart(enabled)
            _notify(
                "Auto Start",
                "Enabled (no terminal popup)" if enabled else "Disabled",
            )

    TrayIcon()
    print("[tray] AppIndicator3 started")
    Gtk.main()


# ==========================================================================
# main
# ==========================================================================
def main():
    print("=" * 60)
    print(f"  StickS3 Claude Codex Helper (Ubuntu/Linux)")
    print(f"  HTTP :{CONFIG['http_port']}   UDP discovery :{CONFIG['udp_port']}")
    print(f"  LAN IP: {local_ip()}")
    print(f"  Log:    {LOG_FILE}")
    print(f"  Config: {CONFIG_PATH}")
    print("=" * 60)

    # HTTP server
    try:
        http_srv = ThreadedHTTPServer(("0.0.0.0", CONFIG["http_port"]), Handler)
    except OSError as e:
        print(f"[error] HTTP port {CONFIG['http_port']} in use: {e}")
        sys.exit(1)
    threading.Thread(target=http_srv.serve_forever, daemon=True).start()

    # UDP discovery
    threading.Thread(target=discovery_loop, daemon=True).start()

    # Tray
    try:
        start_tray()
    except KeyboardInterrupt:
        print("\n[stopped]")


if __name__ == "__main__":
    main()
