"""Dojo launcher: point it at your disc image, set the options, build and play.

In a release it is Dojo.exe beside the source, the game's decompiled source (decomp/) and the bundled toolchain
(toolchain/); in a checkout, run `python launcher.pyw` after `python tools/fetch_toolchain.py` (Play downloads
the decompiled source there). It builds the game with the bundled tools and starts it. It keeps its own choices
in launcher.json, writes the game's options to build/dojo.ini (the file the in-game options menu, F1, changes
too), and runs the game in this folder, so the memory-card saves live in saves/ here.

A release keeps itself up to date: when it opens it reads the latest release's dojo-update.json on GitHub, and
when that names a newer version it downloads it, checks it, and (after the launcher and the game have closed)
a small script puts the new files in place and opens the new launcher. Play then rebuilds the game.
"""

import hashlib
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import urllib.request
import zipfile
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

DOJO_VERSION = "0.1.5"

# The folder of Dojo.exe in a release, of this file in a checkout.
PORT_DIR = os.path.dirname(os.path.abspath(sys.executable if getattr(sys, "frozen", False) else __file__))
TOOLCHAIN = os.path.join(PORT_DIR, "toolchain")
CMAKE = os.path.join(TOOLCHAIN, "cmake", "bin", "cmake.exe")
DECOMP_DIR = os.path.join(PORT_DIR, "decomp")

# The decompilation, with Dojo's changes, this version is built from: a branch of a fork of the decompilation,
# fetched at this exact commit.
DECOMP_COMMIT = "6ed46c44c2c5134a2ff15604e60b744388737c71"
DECOMP_URL = "https://github.com/tisquantum/bushido/archive/%s.zip" % DECOMP_COMMIT
DECOMP_STAMP = ".dojo-decomp"
BUILD_DIR = os.path.join(PORT_DIR, "build")
EXE = os.path.join(BUILD_DIR, "dojo.exe")
INI = os.path.join(BUILD_DIR, "dojo.ini")
LAUNCHER_JSON = os.path.join(PORT_DIR, "launcher.json")
GAME_LOG = os.path.join(PORT_DIR, "dojo.log")
SAVES = os.path.join(PORT_DIR, "saves")

# Updates: the manifest tools/make_release.py writes, uploaded to every release. DOJO_UPDATE_URL points a test at
# another manifest (and turns updates on in a checkout, which otherwise never updates itself: git does that).
UPDATE_MANIFEST = os.environ.get("DOJO_UPDATE_URL") or \
    "https://github.com/tisquantum/dojo/releases/latest/download/dojo-update.json"
UPDATES = getattr(sys, "frozen", False) or bool(os.environ.get("DOJO_UPDATE_URL"))
UPDATE_STAGE = os.path.join(PORT_DIR, "update.new")
UPDATE_LOG = os.path.join(PORT_DIR, "update.log")
UPDATE_FAILED = os.path.join(PORT_DIR, "update.failed")

# Run by cmd once the launcher has gone: waits for its processes (PyInstaller's and Python's) to end, mirrors
# each folder of the new version over this one (so files a version dropped go too), then copies the files at the
# top, Dojo.exe last of all, so a failure part way leaves the old launcher, which tries again.
UPDATE_SCRIPT = r"""@echo off
setlocal
set FAILED=
for %%p in (%DOJO_PIDS%) do call :wait %%p
for /D %%d in ("%DOJO_STAGE%\*") do call :mirror "%%d" "%DOJO_HOME%\%%~nxd"
if defined FAILED goto done
robocopy "%DOJO_STAGE%" "%DOJO_HOME%" /XF Dojo.exe /R:30 /W:1 /NP /NJH /NDL
if errorlevel 8 set FAILED=1
if defined FAILED goto done
robocopy "%DOJO_STAGE%" "%DOJO_HOME%" Dojo.exe /R:30 /W:1 /NP /NJH /NDL
if errorlevel 8 set FAILED=1
:done
if defined FAILED (echo failed> "%DOJO_HOME%\update.failed") else (rmdir /s /q "%DOJO_STAGE%")
if "%DOJO_RELAUNCH%"=="1" start "" /D "%DOJO_HOME%" "%DOJO_HOME%\Dojo.exe"
exit /b

:wait
set /a WAITED=0
:waiting
tasklist /FI "PID eq %1" /NH 2>nul | find " %1 " >nul || exit /b
set /a WAITED+=1
if %WAITED% gtr 60 exit /b
ping -n 2 127.0.0.1 >nul
goto waiting

:mirror
robocopy %1 %2 /MIR /R:30 /W:1 /NP /NJH /NDL /NFL
if errorlevel 8 set FAILED=1
exit /b
"""

# The disc the port is made from (the decompilation's README lists it).
KNOWN_DISCS = {
    "c5cba1e3e8bc68f15e32492907ac37add7f8e8d3e10ddb36742a729ce1a970bf": "Bushido Blade (USA)",
}
SECTOR = 2352
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

ASPECTS = ["auto", "4:3", "16:10", "16:9", "21:9", "24:10", "32:9"]
ASPECT_NAMES = ["Match my display", "4:3 (original)", "16:10", "16:9", "21:9", "24:10", "32:9"]
FPS_VALUES = [0] + list(range(30, 481, 30))
SCALE_NAMES = ["1x (320x240)", "2x (640x480)", "3x (960x720)", "4x (1280x960)", "5x (1600x1200)",
               "6x (1920x1440)", "7x (2240x1680)", "8x (2560x1920)"]


# The look: sumi ink and washi paper, with the crimson of the logo's sword cut.
INK = "#14110f"        # the window
PANEL = "#1c1815"      # the pages
FIELD = "#28221d"      # inputs and buttons
FIELD_HOVER = "#352d26"
LINE = "#3d342b"
PAPER = "#ece4d4"      # text
MUTED = "#a39684"
DIM = "#655a4e"
CRIMSON = "#e5383b"
CRIMSON_DARK = "#b3242b"
GOOD = "#8fbf73"
WARN = "#e3a857"
BAD = "#ff7a6e"

# The logo's letters (tools/make_logo.py): outlines and holes, 150 x 140 units each.
LOGO_LETTERS = {
    "D": "M0,0 H100 L150,50 V90 L100,140 H0 Z M34,34 V106 H86 L116,76 V64 L86,34 Z",
    "O": "M40,0 H110 L150,40 V100 L110,140 H40 L0,100 V40 Z M52,34 L34,52 V88 L52,106 H98 L116,88 V52 L98,34 Z",
    "J": "M50,0 H150 V100 L110,140 H40 L0,100 V78 H34 V88 L52,106 H98 L116,88 V34 H50 Z",
}


def logo_shapes(path):
    """The polygons of a logo letter's path (M, H, V, L and Z only): the outline first, then its hole."""
    shapes, points, x, y = [], [], 0.0, 0.0
    tokens = re.findall(r"[MLHVZ]|-?[0-9.]+", path)
    i = 0
    while i < len(tokens):
        t = tokens[i]
        if t == "M":
            if points:
                shapes.append(points)
            x, y = float(tokens[i + 1]), float(tokens[i + 2])
            points = [(x, y)]
            i += 3
        elif t == "L":
            x, y = float(tokens[i + 1]), float(tokens[i + 2])
            points.append((x, y))
            i += 3
        elif t == "H":
            x = float(tokens[i + 1])
            points.append((x, y))
            i += 2
        elif t == "V":
            y = float(tokens[i + 1])
            points.append((x, y))
            i += 2
        else:   # Z
            i += 1
    if points:
        shapes.append(points)
    return shapes


def draw_logo(canvas, left, top, scale):
    """DOJO with the sword cut through the last O, as on the README."""
    for n, letter in enumerate("DOJO"):
        ox = left + n * (150 + 34) * scale
        for k, shape in enumerate(logo_shapes(LOGO_LETTERS[letter])):
            flat = [v for px, py in shape for v in (ox + px * scale, top + py * scale)]
            canvas.create_polygon(flat, fill=PAPER if k == 0 else INK, outline="")
    lx = left + 3 * (150 + 34) * scale
    cut = (lx - 22 * scale, top + 160 * scale, lx + 172 * scale, top - 20 * scale)
    canvas.create_line(*cut, fill=INK, width=max(3, 22 * scale))
    canvas.create_line(*cut, fill=CRIMSON, width=max(2, 6 * scale), capstyle="round")


def apply_theme(root):
    root.configure(background=INK)
    style = ttk.Style(root)
    style.theme_use("clam")
    base = ("Segoe UI", 10)
    style.configure(".", background=PANEL, foreground=PAPER, fieldbackground=FIELD, bordercolor=LINE,
                    lightcolor=PANEL, darkcolor=PANEL, troughcolor=FIELD, focuscolor=CRIMSON,
                    selectbackground=CRIMSON, selectforeground=PAPER, insertcolor=PAPER, font=base)
    style.configure("TFrame", background=PANEL)
    style.configure("Ink.TFrame", background=INK)
    style.configure("TLabel", background=PANEL, foreground=PAPER)
    style.configure("Ink.TLabel", background=INK, foreground=MUTED)
    style.configure("Kanji.TLabel", background=INK, foreground=CRIMSON_DARK, font=("Yu Gothic UI", 20, "bold"))
    style.configure("Tagline.TLabel", background=INK, foreground=MUTED, font=("Segoe UI", 9))
    style.configure("Section.TLabel", background=PANEL, foreground=CRIMSON, font=("Segoe UI", 10, "bold"))
    style.configure("Sub.TLabel", background=PANEL, foreground=MUTED)
    style.configure("Good.TLabel", background=PANEL, foreground=GOOD)
    style.configure("Bad.TLabel", background=PANEL, foreground=BAD)
    style.configure("Warn.TLabel", background=PANEL, foreground=WARN)
    style.configure("Banner.TFrame", background=FIELD)
    style.configure("Banner.TLabel", background=FIELD, foreground=PAPER)

    style.configure("TNotebook", background=INK, borderwidth=0, tabmargins=(12, 6, 12, 0))
    style.configure("TNotebook.Tab", background=INK, foreground=MUTED, padding=(18, 7), borderwidth=1,
                    bordercolor=LINE, lightcolor=INK, darkcolor=INK, font=("Segoe UI", 10, "bold"))
    style.map("TNotebook.Tab", background=[("selected", PANEL), ("active", FIELD)],
              foreground=[("selected", PAPER), ("active", PAPER)],
              lightcolor=[("selected", CRIMSON)], bordercolor=[("selected", LINE)])

    style.configure("TButton", background=FIELD, foreground=PAPER, bordercolor=LINE, padding=(12, 5),
                    focusthickness=0)
    style.map("TButton", background=[("disabled", PANEL), ("pressed", CRIMSON_DARK), ("active", FIELD_HOVER)],
              foreground=[("disabled", DIM)], bordercolor=[("active", CRIMSON)])
    style.configure("Play.TButton", background=CRIMSON, foreground="#ffffff", bordercolor=CRIMSON,
                    font=("Segoe UI", 12, "bold"), padding=(28, 8))
    style.map("Play.TButton", background=[("disabled", FIELD), ("pressed", CRIMSON_DARK), ("active", "#f04a4d")],
              foreground=[("disabled", DIM)], bordercolor=[("disabled", LINE)])

    style.configure("TEntry", fieldbackground=FIELD, foreground=PAPER, bordercolor=LINE, padding=4)
    style.map("TEntry", fieldbackground=[("readonly", PANEL)], foreground=[("readonly", MUTED)],
              bordercolor=[("focus", CRIMSON)])
    style.configure("TCombobox", fieldbackground=FIELD, background=FIELD, foreground=PAPER, arrowcolor=PAPER,
                    bordercolor=LINE, padding=3)
    style.map("TCombobox", fieldbackground=[("readonly", FIELD)], foreground=[("readonly", PAPER)],
              selectbackground=[("readonly", FIELD)], selectforeground=[("readonly", PAPER)],
              background=[("active", FIELD_HOVER)], bordercolor=[("focus", CRIMSON)])
    root.option_add("*TCombobox*Listbox.background", FIELD)
    root.option_add("*TCombobox*Listbox.foreground", PAPER)
    root.option_add("*TCombobox*Listbox.selectBackground", CRIMSON)
    root.option_add("*TCombobox*Listbox.selectForeground", "#ffffff")
    root.option_add("*TCombobox*Listbox.font", base)

    style.configure("TCheckbutton", background=PANEL, foreground=PAPER, indicatorbackground=FIELD,
                    indicatorforeground=PAPER, focusthickness=0)
    style.map("TCheckbutton", background=[("active", PANEL)],
              indicatorbackground=[("selected", CRIMSON), ("active", FIELD_HOVER)])
    style.configure("Horizontal.TScale", background=CRIMSON, lightcolor=CRIMSON, darkcolor=CRIMSON,
                    bordercolor=CRIMSON_DARK, troughcolor=FIELD, sliderlength=14, gripcount=0)
    style.map("Horizontal.TScale", background=[("active", "#f04a4d")])
    style.configure("Horizontal.TProgressbar", background=CRIMSON, troughcolor=FIELD, bordercolor=LINE)
    style.configure("TSeparator", background=LINE)
    style.configure("Vertical.TScrollbar", background=FIELD_HOVER, lightcolor=FIELD_HOVER, darkcolor=FIELD_HOVER,
                    troughcolor="#0e0c0a", arrowcolor=MUTED, bordercolor=PANEL, gripcount=0)
    style.map("Vertical.TScrollbar", background=[("active", LINE)], lightcolor=[("active", LINE)],
              darkcolor=[("active", LINE)])


def dark_title_bar(root):
    """Windows 10/11: a dark title bar to match."""
    try:
        import ctypes
        root.update_idletasks()
        hwnd = ctypes.windll.user32.GetParent(root.winfo_id())
        on = ctypes.c_int(1)
        for attribute in (20, 19):   # DWMWA_USE_IMMERSIVE_DARK_MODE (and its pre-20H1 number)
            if ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, attribute, ctypes.byref(on), 4) == 0:
                break
    except (AttributeError, OSError):
        pass


def default_name():
    name = "".join(c for c in os.environ.get("USERNAME", "") if c.isalnum() or c in "_-")[:23]
    return name or "Samurai"


# The game's options, in the order and with the defaults of port/src/settings.c.
DEFAULTS = {
    "aspect": "auto", "name": default_name(), "server": "dojo-matchmaker.fly.dev:7777",
    "fps": 0, "scale": 4, "software": 0, "fullscreen": 0, "vsync": 0, "volume": 100, "show_fps": 0,
    "analog_stick": 0, "p2p_port": 7778, "watch_hill": 1, "offline": 0, "stick_deadzone": 50, "rumble": 100,
    "dualsense_haptics": 1, "keyboard_player": 1,
}


def load_ini():
    values = dict(DEFAULTS)
    try:
        with open(INI, encoding="utf-8", errors="replace") as f:
            for line in f:
                if "=" not in line or line.lstrip().startswith(";"):
                    continue
                key, value = (part.strip() for part in line.split("=", 1))
                if key not in values or not value:
                    continue
                if isinstance(DEFAULTS[key], int):
                    try:
                        values[key] = int(value)
                    except ValueError:
                        pass
                else:
                    values[key] = value.split()[0]
    except OSError:
        pass
    return values


def ini_stamp():
    """When dojo.ini last changed, to notice the game changing it."""
    try:
        st = os.stat(INI)
        return st.st_mtime_ns, st.st_size
    except OSError:
        return None


def save_ini(values):
    os.makedirs(BUILD_DIR, exist_ok=True)
    lines = ["; Dojo: the options menu (F1) writes this file.\n"]
    lines += ["%s = %s\n" % (key, values[key]) for key in DEFAULTS]
    # The game's own settings the launcher doesn't show (the controls page's bindings and gamepads) stay.
    try:
        with open(INI, encoding="utf-8", errors="replace") as f:
            lines += [line if line.endswith("\n") else line + "\n" for line in f
                      if "=" in line and not line.lstrip().startswith(";")
                      and line.split("=", 1)[0].strip() not in DEFAULTS]
    except OSError:
        pass
    with open(INI, "w", encoding="utf-8") as f:
        f.writelines(lines)


# -- the controls (port/src/input.c) ---------------------------------------------------------------------------

# A binding: what one slot of a console button is worked by (input.h), 0 for nothing.
KEY, MOUSE, PAD, AXIS = 0x10000, 0x20000, 0x30000, 0x40000

# The console's buttons, in the game's order, with their dojo.ini keys.
BUTTONS = [("Cross", "cross"), ("Circle", "circle"), ("Square", "square"), ("Triangle", "triangle"),
           ("L1", "l1"), ("R1", "r1"), ("L2", "l2"), ("R2", "r2"), ("Start", "start"), ("Select", "select"),
           ("D-pad up", "up"), ("D-pad down", "down"), ("D-pad left", "left"), ("D-pad right", "right")]

# SDL's scancodes, their names (SDL_GetScancodeName) and the Windows virtual keys Tk reports for them.
SCANCODES = {}   # scancode: name
VK_SCANCODES = {}  # virtual key: scancode
for _i in range(26):
    SCANCODES[4 + _i] = chr(65 + _i)
    VK_SCANCODES[65 + _i] = 4 + _i
for _i in range(10):
    SCANCODES[30 + (_i + 9) % 10] = str(_i)
    VK_SCANCODES[48 + _i] = 30 + (_i + 9) % 10
    SCANCODES[89 + (_i + 9) % 10] = "Keypad %d" % _i
    VK_SCANCODES[96 + _i] = 89 + (_i + 9) % 10
for _i in range(12):
    SCANCODES[58 + _i] = "F%d" % (_i + 1)
    VK_SCANCODES[112 + _i] = 58 + _i
for _vk, _code, _name in [
        (8, 42, "Backspace"), (9, 43, "Tab"), (13, 40, "Return"), (19, 72, "Pause"), (20, 57, "CapsLock"),
        (27, 41, "Escape"), (32, 44, "Space"), (33, 75, "PageUp"), (34, 78, "PageDown"), (35, 77, "End"),
        (36, 74, "Home"), (37, 80, "Left"), (38, 82, "Up"), (39, 79, "Right"), (40, 81, "Down"), (45, 73, "Insert"),
        (46, 76, "Delete"), (106, 85, "Keypad *"), (107, 87, "Keypad +"), (109, 86, "Keypad -"),
        (110, 99, "Keypad ."), (111, 84, "Keypad /"), (144, 83, "Numlock"), (145, 71, "ScrollLock"),
        (186, 51, ";"), (187, 46, "="), (188, 54, ","), (189, 45, "-"), (190, 55, "."), (191, 56, "/"),
        (192, 53, "`"), (219, 47, "["), (220, 49, "\\"), (221, 48, "]"), (222, 52, "'"), (91, 227, "Left GUI"),
        (92, 231, "Right GUI")]:
    SCANCODES[_code] = _name
    VK_SCANCODES[_vk] = _code
SCANCODES.update({88: "Keypad Enter", 224: "Left Ctrl", 225: "Left Shift", 226: "Left Alt", 228: "Right Ctrl",
                  229: "Right Shift", 230: "Right Alt"})
# Tk's mouse buttons to SDL's (left, middle, right, then the two side buttons).
MOUSE_BUTTONS = {1: 1, 2: 2, 3: 3, 4: 4, 5: 5}

DEFAULT_BINDS = [[PAD | 0, 0, KEY | 27, 0], [PAD | 1, 0, KEY | 6, 0], [PAD | 2, 0, KEY | 29, 0],
                 [PAD | 3, 0, KEY | 22, 0], [PAD | 9, 0, KEY | 20, 0], [PAD | 10, 0, KEY | 8, 0],
                 [AXIS | 8, 0, KEY | 30, 0], [AXIS | 10, 0, KEY | 32, 0], [PAD | 6, 0, KEY | 40, 0],
                 [PAD | 4, 0, KEY | 42, 0], [PAD | 11, 0, KEY | 82, 0], [PAD | 12, 0, KEY | 81, 0],
                 [PAD | 13, 0, KEY | 80, 0], [PAD | 14, 0, KEY | 79, 0]]


class Gamepads:
    """The gamepads, seen by the launcher itself through a 64-bit SDL3.dll packaged inside Dojo.exe (in a checkout,
    the one tools/make_release.py unpacked into dist/_pyinstaller/sdl64): the same SDL the game uses, so the same
    IDs, kinds and button numbers. Kept open only while the Controls tab is showing and the game isn't running."""

    INIT_GAMEPAD = 0x2000
    WIRED = 1
    BUTTONS = 26   # SDL_GAMEPAD_BUTTON_COUNT
    GUIDE = 5

    def __init__(self):
        self.sdl = None
        self.open = {}   # SDL_JoystickID: SDL_Gamepad *
        path = os.path.join(getattr(sys, "_MEIPASS", ""), "SDL3.dll") if getattr(sys, "frozen", False) else \
            os.path.join(PORT_DIR, "dist", "_pyinstaller", "sdl64", "SDL3.dll")
        try:
            import ctypes
            sdl = ctypes.CDLL(path)
        except OSError:
            return
        c = ctypes

        class GUID(c.Structure):
            _fields_ = [("data", c.c_uint8 * 16)]

        for name, res, args in [
                ("SDL_SetHint", c.c_bool, [c.c_char_p, c.c_char_p]), ("SDL_Init", c.c_bool, [c.c_uint32]),
                ("SDL_GetGamepads", c.POINTER(c.c_uint32), [c.POINTER(c.c_int)]), ("SDL_free", None, [c.c_void_p]),
                ("SDL_GetGamepadNameForID", c.c_char_p, [c.c_uint32]),
                ("SDL_GetGamepadGUIDForID", GUID, [c.c_uint32]),
                ("SDL_GUIDToString", None, [GUID, c.c_char_p, c.c_int]),
                ("SDL_GetGamepadTypeForID", c.c_int, [c.c_uint32]),
                ("SDL_GetGamepadStringForType", c.c_char_p, [c.c_int]),
                ("SDL_OpenGamepad", c.c_void_p, [c.c_uint32]), ("SDL_CloseGamepad", None, [c.c_void_p]),
                ("SDL_GetGamepadConnectionState", c.c_int, [c.c_void_p]), ("SDL_UpdateGamepads", None, []),
                ("SDL_PumpEvents", None, []),
                ("SDL_GetGamepadButton", c.c_bool, [c.c_void_p, c.c_int]),
                ("SDL_GetGamepadAxis", c.c_int16, [c.c_void_p, c.c_int]),
                ("SDL_RumbleGamepad", c.c_bool, [c.c_void_p, c.c_uint16, c.c_uint16, c.c_uint32])]:
            f = getattr(sdl, name)
            f.restype, f.argtypes = res, args
        sdl.SDL_SetHint(b"SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", b"1")
        if sdl.SDL_Init(self.INIT_GAMEPAD):
            self.sdl, self.c = sdl, c

    def available(self):
        return self.sdl is not None

    def list(self):
        """The connected gamepads, as dojo.exe --pads lists them: (guid, kind, wired, name)."""
        if not self.sdl:
            return []
        c, sdl = self.c, self.sdl
        sdl.SDL_PumpEvents()
        count = c.c_int(0)
        ids = sdl.SDL_GetGamepads(c.byref(count))
        found = [ids[i] for i in range(count.value)] if ids else []
        if ids:
            sdl.SDL_free(ids)
        for gone in [i for i in self.open if i not in found]:
            sdl.SDL_CloseGamepad(self.open.pop(gone))
        pads, self.order = [], []
        for i in found:
            if i not in self.open:
                pad = sdl.SDL_OpenGamepad(i)
                if not pad:
                    continue
                self.open[i] = pad
            text = c.create_string_buffer(40)
            sdl.SDL_GUIDToString(sdl.SDL_GetGamepadGUIDForID(i), text, 40)
            kind = sdl.SDL_GetGamepadStringForType(sdl.SDL_GetGamepadTypeForID(i)) or b"standard"
            self.order.append(i)
            pads.append((text.value.decode(), kind.decode(),
                         sdl.SDL_GetGamepadConnectionState(self.open[i]) == self.WIRED,
                         (sdl.SDL_GetGamepadNameForID(i) or b"Gamepad").decode("utf-8", "replace")))
        return pads

    def close(self):
        for pad in self.open.values():
            self.sdl.SDL_CloseGamepad(pad)
        self.open = {}

    def state(self):
        """Every open gamepad's buttons and axes, for spotting a press."""
        self.sdl.SDL_UpdateGamepads()
        return {i: ([self.sdl.SDL_GetGamepadButton(p, b) for b in range(self.BUTTONS)],
                    [self.sdl.SDL_GetGamepadAxis(p, a) for a in range(6)]) for i, p in self.open.items()}

    @classmethod
    def pressed(cls, before, now):
        """A binding (input.h) for what went down between two states, or 0."""
        for i, (buttons, axes) in now.items():
            old_buttons, old_axes = before.get(i, ([False] * cls.BUTTONS, [0] * 6))
            for b, down in enumerate(buttons):
                if down and not old_buttons[b] and b != cls.GUIDE:
                    return PAD | b
            for a, value in enumerate(axes):
                if abs(value) > 24000 and abs(old_axes[a]) < 12000 and (a < 4 or value > 0):
                    return AXIS | (a << 1) | (value < 0)
        return 0

    def rumble(self, index, seconds):
        """Rumbles the gamepad at this place in the last list()."""
        pad = self.open.get(self.order[index]) if 0 <= index < len(getattr(self, "order", [])) else None
        return bool(pad) and self.sdl.SDL_RumbleGamepad(pad, 0xFFFF, 0xC000, int(seconds * 1000))


def scancode_for(event):
    """The SDL scancode of a Tk key press, telling left and right modifiers apart."""
    side = {"Shift_L": 225, "Shift_R": 229, "Control_L": 224, "Control_R": 228, "Alt_L": 226, "Alt_R": 230}
    if event.keysym in side:
        return side[event.keysym]
    if event.keysym == "KP_Enter":
        return 88
    return VK_SCANCODES.get(event.keycode, 0)


def binding_name(binding, pad_type):
    """A binding's name as the game shows it, a gamepad's in the words of its kind (SDL's type names)."""
    kind, code = binding & 0xF0000, binding & 0xFFFF
    ps, nintendo = pad_type in ("ps3", "ps4", "ps5"), pad_type.startswith(("switch", "joycon"))
    if kind == KEY:
        return SCANCODES.get(code, "Key %d" % code)
    if kind == MOUSE:
        return {1: "Left click", 2: "Middle click", 3: "Right click"}.get(code, "Mouse %d" % code)
    if kind == PAD:
        faces = (["Cross", "Circle", "Square", "Triangle"] if ps else ["B", "A", "Y", "X"] if nintendo
                 else ["A", "B", "X", "Y"])
        names = faces + [
            {"ps5": "Create", "ps4": "Share", "ps3": "Select", "xboxone": "View"}.get(pad_type,
                                                                                    "Minus" if nintendo else "Back"),
            "PS" if ps else "Home" if nintendo else "Guide",
            "Options" if ps and pad_type != "ps3" else "Plus" if nintendo else "Menu" if pad_type == "xboxone"
            else "Start",
            "L3" if ps else "LS press", "R3" if ps else "RS press",
            "L1" if ps else "L" if nintendo else "LB", "R1" if ps else "R" if nintendo else "RB",
            "D-pad up", "D-pad down", "D-pad left", "D-pad right",
            "Mic" if pad_type == "ps5" else "Capture" if nintendo else "Share",
            "Paddle P1", "Paddle P3", "Paddle P2", "Paddle P4", "Touchpad"]
        return names[code] if code < len(names) else "Button %d" % code
    if kind == AXIS:
        axis, negative = code >> 1, code & 1
        if axis >= 4:
            right = axis == 5
            return ("R2" if right else "L2") if ps else ("ZR" if right else "ZL") if nintendo else \
                ("RT" if right else "LT")
        way = ("up" if negative else "down") if axis in (1, 3) else ("left" if negative else "right")
        return "%s %s" % ("LS" if axis < 2 else "RS", way)
    return "None"


def load_controls():
    """Each player's gamepad and the bindings, from dojo.ini (the game's defaults where it has none)."""
    controls = {"pad1": "auto", "pad2": "auto", "pad1_name": "", "pad2_name": "",
                "binds": [list(b) for b in DEFAULT_BINDS]}
    keys = {"bind_" + key: i for i, (_, key) in enumerate(BUTTONS)}
    try:
        with open(INI, encoding="utf-8", errors="replace") as f:
            for line in f:
                if "=" not in line or line.lstrip().startswith(";"):
                    continue
                key, value = (part.strip() for part in line.split("=", 1))
                if key in ("pad1", "pad2", "pad1_name", "pad2_name") and value:
                    controls[key] = value
                elif key in keys:
                    try:
                        binds = [int(v, 16) for v in value.split()]
                    except ValueError:
                        continue
                    if len(binds) == 4:
                        controls["binds"][keys[key]] = binds
    except OSError:
        pass
    return controls


def save_controls(controls):
    """Writes the gamepads and bindings into dojo.ini, the rest of it as it was."""
    lines = {"pad1": controls["pad1"], "pad2": controls["pad2"], "pad1_name": controls["pad1_name"],
             "pad2_name": controls["pad2_name"]}
    for i, (_, key) in enumerate(BUTTONS):
        lines["bind_" + key] = " ".join("%x" % b for b in controls["binds"][i])
    out = []
    try:
        with open(INI, encoding="utf-8", errors="replace") as f:
            for line in f:
                key = line.split("=", 1)[0].strip() if "=" in line else None
                if key in lines:
                    out.append("%s = %s\n" % (key, lines.pop(key)))
                else:
                    out.append(line if line.endswith("\n") else line + "\n")
    except OSError:
        out.append("; Dojo: the options menu (F1) writes this file.\n")
    out += ["%s = %s\n" % (key, value) for key, value in lines.items()]
    os.makedirs(BUILD_DIR, exist_ok=True)
    with open(INI, "w", encoding="utf-8") as f:
        f.writelines(out)


# -- the disc -------------------------------------------------------------------------------------------------

def cue_tracks(cue):
    """The files a .cue names, resolved beside it."""
    files = []
    with open(cue, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line.upper().startswith("FILE "):
                continue
            rest = line[5:].strip()
            if rest.startswith('"'):
                name = rest[1:].split('"', 1)[0]
            else:
                name = rest.rsplit(" ", 1)[0]
            files.append(os.path.join(os.path.dirname(cue), name))
    return files


def resolve_disc(path):
    """The .bin to run from a .bin, a .cue or a folder holding them: (bin path or None, a note for the player)."""
    if not path:
        return None, "Choose your disc image: a .cue, a .bin or the folder they're in."
    if os.path.isdir(path):
        entries = sorted(os.listdir(path))
        cues = [os.path.join(path, e) for e in entries if e.lower().endswith(".cue")]
        bins = [os.path.join(path, e) for e in entries if e.lower().endswith(".bin")]
        if cues:
            return resolve_disc(cues[0])
        if bins:
            return max(bins, key=os.path.getsize), "Using the .bin in that folder (no .cue there)."
        return None, "That folder has no .cue or .bin in it."
    if not os.path.isfile(path):
        return None, "That file doesn't exist any more."
    if path.lower().endswith(".cue"):
        tracks = cue_tracks(path)
        if not tracks:
            return None, "That .cue doesn't name a .bin."
        if os.path.isfile(tracks[0]):
            note = "From the .cue." if len(tracks) == 1 else \
                "From the .cue: %d tracks, the game reads the first (its data track)." % len(tracks)
            return tracks[0], note
        # The .bin was renamed after the .cue was made: one .bin beside it is the one.
        bins = [os.path.join(os.path.dirname(path), e) for e in os.listdir(os.path.dirname(path))
                if e.lower().endswith(".bin")]
        if len(bins) == 1:
            return bins[0], "The .cue names \"%s\", which isn't there; using the .bin beside it." % \
                os.path.basename(tracks[0])
        return None, "The .cue names \"%s\", which isn't there." % os.path.basename(tracks[0])
    return path, ""


def looks_like_disc(bin_path):
    """A raw (2352-byte sector) PlayStation CD image: the ISO 9660 volume descriptor where it belongs."""
    try:
        size = os.path.getsize(bin_path)
        with open(bin_path, "rb") as f:
            f.seek(16 * SECTOR + 24)
            descriptor = f.read(6)
    except OSError as e:
        return False, str(e)
    if size % SECTOR:
        return False, "This isn't a raw CD image (2352-byte sectors). Use the .bin/.cue rip, not an .iso."
    if descriptor != b"\x01CD001":
        return False, "No CD file system found in this image."
    return True, "%.0f MB" % (size / 1e6)


# -- the build tools ------------------------------------------------------------------------------------------

def toolchain_version():
    """The bundled toolchain's version, or None when it isn't there."""
    try:
        with open(os.path.join(TOOLCHAIN, "TOOLCHAIN_VERSION")) as f:
            return f.read().strip()
    except OSError:
        return None


def version_key(text):
    return tuple(int(n) for n in re.findall(r"\d+", text))


def extract(archive, dest, prefix=""):
    """Unpacks the zip's files under prefix into dest, refusing any that would land outside it."""
    root = os.path.normpath(dest)
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            if not info.filename.startswith(prefix) or info.filename.endswith("/"):
                continue
            target = os.path.normpath(os.path.join(root, info.filename[len(prefix):]))
            if not target.startswith(root + os.sep):
                raise OSError("unsafe path in the download: " + info.filename)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with z.open(info) as src, open(target, "wb") as out:
                shutil.copyfileobj(src, out)


def decomp_commit(path):
    """The commit a downloaded decompilation came from (None for one chosen by hand)."""
    try:
        with open(os.path.join(path, DECOMP_STAMP)) as f:
            return f.read().strip()
    except OSError:
        return None


# -- the window -----------------------------------------------------------------------------------------------

class Launcher:
    def __init__(self, root):
        self.root = root
        self.events = queue.Queue()
        self.busy = None          # "build", "verify" or None
        self.game = None          # the running game's process
        self.config = self.load_config()

        root.title("Dojo %s" % DOJO_VERSION)
        root.minsize(760, 620)
        apply_theme(root)
        dark_title_bar(root)

        header = ttk.Frame(root, style="Ink.TFrame", padding=(20, 14, 20, 6))
        header.pack(fill="x")
        logo = tk.Canvas(header, width=250, height=58, background=INK, highlightthickness=0)
        logo.pack(side="left")
        draw_logo(logo, 4, 9, 0.29)
        words = ttk.Frame(header, style="Ink.TFrame")
        words.pack(side="left", padx=(16, 0), pady=(6, 0))
        ttk.Label(words, text="B U S H I D O   B L A D E   ·   P C", style="Ink.TLabel",
                  font=("Segoe UI", 9, "bold")).pack(anchor="w")
        ttk.Label(words, text="High frame rates  ·  any aspect ratio  ·  online rollback play",
                  style="Tagline.TLabel").pack(anchor="w", pady=(2, 0))
        ttk.Label(header, text="武士道", style="Kanji.TLabel").pack(side="right")
        tk.Frame(root, background=CRIMSON_DARK, height=2).pack(fill="x", padx=20)

        # A newer version: shown once the check finds one.
        self.update = None            # the newer release's manifest
        self.update_state = None      # None, "downloading", "ready" (staged in update.new) or "failed"
        self.update_wanted = False    # install as soon as it's staged and nothing is running
        self.update_blocked = False   # the last install failed: don't install by itself again
        self.banner = ttk.Frame(root, style="Banner.TFrame", padding=(12, 6))
        self.banner_text = ttk.Label(self.banner, text="", style="Banner.TLabel")
        self.banner_text.pack(side="left")
        ttk.Button(self.banner, text="What's new", command=self.open_release_notes).pack(side="right")
        self.update_button = ttk.Button(self.banner, text="Update now", command=self.update_now)
        self.update_button.pack(side="right", padx=6)

        self.tabs = ttk.Notebook(root)
        self.tabs.pack(fill="both", expand=True, padx=12, pady=(8, 6))
        self.build_game_tab()
        self.build_settings_tab()
        self.build_build_tab()
        self.build_controls_tab()
        self.shown_settings = self.settings()   # dojo.ini as the tabs show it
        self.ini_stamp = ini_stamp()
        root.bind("<FocusIn>", lambda e: self.reload_settings() if e.widget is root else None, add="+")

        bar = ttk.Frame(root, style="Ink.TFrame", padding=(20, 6, 20, 14))
        bar.pack(fill="x")
        self.status = ttk.Label(bar, text="", style="Ink.TLabel")
        self.status.pack(side="left", fill="x", expand=True)
        self.play_button = ttk.Button(bar, text="Play", style="Play.TButton", command=self.play)
        self.play_button.pack(side="right")
        self.build_button = ttk.Button(bar, text="Build", command=self.build)
        self.build_button.pack(side="right", padx=8)

        root.protocol("WM_DELETE_WINDOW", self.close)
        self.refresh_disc()
        self.refresh_decomp()
        self.check_tools()
        self.update_buttons()
        self.start_update_check()
        root.after(50, self.poll)

    # -- the launcher's own settings --

    def load_config(self):
        config = {"disc": "", "decomp": "", "build_type": "Release", "build_first": True, "auto_update": True,
                  "built_version": ""}
        try:
            with open(LAUNCHER_JSON, encoding="utf-8") as f:
                saved = json.load(f)
            if saved.get("version") != 2:
                saved.pop("build_type", None)   # players build Release: the same bytes as everyone else
            config.update(saved)
        except (OSError, ValueError):
            pass
        config["version"] = 2
        if not config["decomp"]:
            parent = os.path.dirname(PORT_DIR)
            if os.path.isfile(os.path.join(parent, "src", "main.c")):
                config["decomp"] = parent   # a checkout inside the decompilation
            else:
                config["decomp"] = DECOMP_DIR
        if not config["disc"]:
            for folder in (os.path.join(config["decomp"] or os.path.dirname(PORT_DIR), "game_files"),
                           os.path.join(PORT_DIR, "game_files")):
                if os.path.isdir(folder) and resolve_disc(folder)[0]:
                    config["disc"] = folder
                    break
        return config

    def save_config(self):
        try:
            with open(LAUNCHER_JSON, "w", encoding="utf-8") as f:
                json.dump(self.config, f, indent=2)
        except OSError:
            pass

    # -- Game tab --

    def build_game_tab(self):
        tab = ttk.Frame(self.tabs, padding=16)
        self.tabs.add(tab, text="  Game  ")
        tab.columnconfigure(1, weight=1)

        ttk.Label(tab, text="Disc image", style="Section.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(tab, text="Your own copy of Bushido Blade (USA), ripped as .bin/.cue. Pick the .cue, the "
                  ".bin, or the folder they're in; the file names don't matter.", style="Sub.TLabel",
                  wraplength=640).grid(row=1, column=0, columnspan=4, sticky="w", pady=(0, 6))
        self.disc_var = tk.StringVar(value=self.config["disc"])
        ttk.Entry(tab, textvariable=self.disc_var).grid(row=2, column=0, columnspan=2, sticky="ew")
        ttk.Button(tab, text="File...", command=self.pick_disc_file).grid(row=2, column=2, padx=(6, 0))
        ttk.Button(tab, text="Folder...", command=self.pick_disc_folder).grid(row=2, column=3, padx=(6, 0))
        self.disc_var.trace_add("write", lambda *_: self.refresh_disc())
        self.disc_info = ttk.Label(tab, text="", wraplength=640)
        self.disc_info.grid(row=3, column=0, columnspan=3, sticky="w", pady=(6, 0))
        self.verify_button = ttk.Button(tab, text="Verify disc", command=self.verify_disc)
        self.verify_button.grid(row=3, column=3, sticky="e", pady=(6, 0))
        self.verify_progress = ttk.Progressbar(tab, maximum=1000)
        self.verify_progress.grid(row=4, column=0, columnspan=4, sticky="ew", pady=(6, 0))
        self.verify_progress.grid_remove()

        ttk.Separator(tab).grid(row=5, column=0, columnspan=4, sticky="ew", pady=16)

        self.exe_info = ttk.Label(tab, text="")
        self.exe_info.grid(row=6, column=0, columnspan=4, sticky="w")
        self.build_first_var = tk.BooleanVar(value=self.config["build_first"])
        ttk.Checkbutton(tab, text="Build before playing (picks up changes; quick when nothing changed)",
                        variable=self.build_first_var,
                        command=lambda: self.set_config("build_first", self.build_first_var.get())
                        ).grid(row=7, column=0, columnspan=4, sticky="w", pady=(8, 0))
        if UPDATES:
            self.auto_update_var = tk.BooleanVar(value=self.config["auto_update"])
            ttk.Checkbutton(tab, text="Keep Dojo up to date (installs new versions from GitHub when it opens)",
                            variable=self.auto_update_var,
                            command=lambda: self.set_config("auto_update", self.auto_update_var.get())
                            ).grid(row=8, column=0, columnspan=4, sticky="w", pady=(4, 0))

    def set_config(self, key, value):
        self.config[key] = value
        self.save_config()

    def pick_disc_file(self):
        path = filedialog.askopenfilename(title="Choose the disc image",
                                          filetypes=[("Disc image", "*.cue *.bin"), ("All files", "*.*")])
        if path:
            self.disc_var.set(os.path.normpath(path))

    def pick_disc_folder(self):
        path = filedialog.askdirectory(title="Choose the folder with the .cue/.bin")
        if path:
            self.disc_var.set(os.path.normpath(path))

    def pick_decomp(self):
        path = filedialog.askdirectory(title="Choose the decompilation folder")
        if path:
            self.decomp_var.set(os.path.normpath(path))

    def disc_bin(self):
        return resolve_disc(self.disc_var.get().strip().strip('"'))[0]

    def refresh_disc(self):
        path = self.disc_var.get().strip().strip('"')
        self.set_config("disc", path)
        bin_path, note = resolve_disc(path)
        if bin_path:
            ok, detail = looks_like_disc(bin_path)
            if ok:
                text = "%s  (%s)%s" % (os.path.basename(bin_path), detail, "\n" + note if note else "")
                self.disc_info.configure(text=text, style="TLabel")
            else:
                self.disc_info.configure(text=detail, style="Bad.TLabel")
                bin_path = None
        else:
            self.disc_info.configure(text=note, style="Bad.TLabel" if path else "Warn.TLabel")
        if hasattr(self, "play_button"):
            self.update_buttons()

    def decomp_ok(self):
        path = self.decomp_var.get().strip().strip('"')
        return bool(path) and os.path.isfile(os.path.join(path, "src", "main.c")) and \
            os.path.isfile(os.path.join(path, "include", "substep.h"))

    def refresh_decomp(self):
        path = self.decomp_var.get().strip().strip('"')
        self.set_config("decomp", path)
        commit = decomp_commit(path) if path else None
        if self.decomp_ok() and commit and commit != DECOMP_COMMIT:
            self.decomp_info.configure(text="Another version's source: Play downloads this version's.",
                                       style="Warn.TLabel")
        elif self.decomp_ok():
            self.decomp_info.configure(text="Included with this release." if commit else
                                       "Your own copy of the decompilation.", style="Good.TLabel")
        elif path and os.path.isfile(os.path.join(path, "src", "main.c")):
            self.decomp_info.configure(text="This decompilation is missing the port's changes "
                                       "(include/substep.h).", style="Bad.TLabel")
        elif path and os.path.normcase(os.path.abspath(path)) == os.path.normcase(DECOMP_DIR) and \
                not os.path.isdir(path):
            self.decomp_info.configure(text="Not here (a checkout of the repository): Play downloads this "
                                       "version's copy from GitHub (a few MB).", style="Sub.TLabel")
        else:
            self.decomp_info.configure(text="Not the decompilation (no src/main.c in it).",
                                       style="Bad.TLabel" if path else "Warn.TLabel")
        if hasattr(self, "play_button"):
            self.update_buttons()

    def download_decomp(self, then_play=False):
        if self.busy:
            return
        self.download_then_play = then_play
        dest = DECOMP_DIR
        if os.path.isdir(dest) and os.listdir(dest) and decomp_commit(dest) is None:
            messagebox.showwarning("Dojo", "%s holds files the launcher didn't download; move them away first."
                                   % dest)
            return
        self.busy = "download"
        self.update_buttons()
        self.decomp_info.configure(text="Downloading...", style="TLabel")
        self.status.configure(text="Downloading the game's source...")

        def work():
            part = dest + ".zip.part"
            try:
                with urllib.request.urlopen(DECOMP_URL) as response, open(part, "wb") as out:
                    done = 0
                    while True:
                        chunk = response.read(1 << 18)
                        if not chunk:
                            break
                        out.write(chunk)
                        done += len(chunk)
                        self.events.put(("download-progress", done))
                fresh = dest + ".new"
                shutil.rmtree(fresh, ignore_errors=True)
                extract(part, fresh, "bushido-%s/" % DECOMP_COMMIT)
                with open(os.path.join(fresh, DECOMP_STAMP), "w") as f:
                    f.write(DECOMP_COMMIT + "\n")
                shutil.rmtree(dest, ignore_errors=True)
                os.replace(fresh, dest)
                self.events.put(("download-done", None))
            except Exception as e:   # network, disk or a bad archive: all reported the same way
                self.events.put(("download-done", str(e)))
            finally:
                if os.path.exists(part):
                    os.remove(part)

        threading.Thread(target=work, daemon=True).start()

    def verify_disc(self):
        bin_path = self.disc_bin()
        if not bin_path or self.busy:
            return
        self.busy = "verify"
        self.verify_progress.grid()
        self.verify_progress["value"] = 0
        self.update_buttons()

        def work():
            digest = hashlib.sha256()
            size = os.path.getsize(bin_path)
            done = 0
            try:
                with open(bin_path, "rb") as f:
                    while True:
                        chunk = f.read(1 << 22)
                        if not chunk:
                            break
                        digest.update(chunk)
                        done += len(chunk)
                        self.events.put(("verify-progress", done * 1000 // max(size, 1)))
                self.events.put(("verify-done", digest.hexdigest()))
            except OSError as e:
                self.events.put(("verify-done", "error: %s" % e))

        threading.Thread(target=work, daemon=True).start()

    # -- Settings tab --

    def build_settings_tab(self):
        tab = ttk.Frame(self.tabs, padding=16)
        self.tabs.add(tab, text="  Settings  ")
        values = load_ini()
        self.vars = {}
        tab.columnconfigure(1, weight=1)
        tab.columnconfigure(3, weight=1)

        def section(row, col, title):
            ttk.Label(tab, text=title, style="Section.TLabel").grid(
                row=row, column=col, columnspan=2, sticky="w", pady=(10 if row else 0, 4))

        def combo(row, col, label, key, names, choices):
            ttk.Label(tab, text=label).grid(row=row, column=col, sticky="w", padx=(0, 8), pady=3)
            var = tk.StringVar()
            current = values[key]
            var.set(names[choices.index(current)] if current in choices else names[0])
            box = ttk.Combobox(tab, textvariable=var, values=names, state="readonly", width=22)
            box.grid(row=row, column=col + 1, sticky="w", padx=(0, 24))
            box.bind("<<ComboboxSelected>>", lambda _: self.save_settings())
            self.vars[key] = (lambda v=var: choices[names.index(v.get())],
                              lambda x, v=var: v.set(names[choices.index(x)] if x in choices else names[0]))

        def check(row, col, label, key):
            var = tk.BooleanVar(value=bool(values[key]))
            ttk.Checkbutton(tab, text=label, variable=var, command=self.save_settings).grid(
                row=row, column=col, columnspan=2, sticky="w", pady=3)
            self.vars[key] = (lambda v=var: int(v.get()), lambda x, v=var: v.set(bool(x)))

        def entry(row, col, label, key, width=26, kind=str):
            ttk.Label(tab, text=label).grid(row=row, column=col, sticky="w", padx=(0, 8), pady=3)
            var = tk.StringVar(value=str(values[key]))
            box = ttk.Entry(tab, textvariable=var, width=width)
            box.grid(row=row, column=col + 1, sticky="w", padx=(0, 24))
            box.bind("<FocusOut>", lambda _: self.save_settings())
            self.vars[key] = (lambda v=var, d=DEFAULTS[key]: self.clean(v.get(), d, kind),
                              lambda x, v=var: v.set(str(x)))

        section(0, 0, "Display")
        combo(1, 0, "Frame rate", "fps", ["Match my display"] + ["%d fps" % f for f in FPS_VALUES[1:]],
              FPS_VALUES)
        combo(2, 0, "Aspect ratio", "aspect", ASPECT_NAMES, ASPECTS)
        combo(3, 0, "Renderer", "software", ["Vulkan (GPU)", "Software (CPU)"], [0, 1])
        combo(4, 0, "Resolution (Vulkan)", "scale", SCALE_NAMES, list(range(1, 9)))
        check(5, 0, "Full screen  (F11 switches in game)", "fullscreen")
        check(6, 0, "V-sync", "vsync")
        check(7, 0, "Show the frame rate", "show_fps")

        section(0, 2, "Sound and controls")
        ttk.Label(tab, text="Volume").grid(row=1, column=2, sticky="w", padx=(0, 8))
        volume = tk.IntVar(value=values["volume"])
        volume_label = ttk.Label(tab, text="%d%%" % volume.get(), width=5)
        ttk.Scale(tab, from_=0, to=100, variable=volume, length=150,
                  command=lambda v: volume_label.configure(text="%d%%" % float(v))).grid(row=1, column=3,
                                                                                         sticky="w")
        volume_label.grid(row=1, column=3, sticky="e", padx=(0, 24))
        tab.bind_all("<ButtonRelease-1>", lambda _: self.save_settings(), add="+")
        self.vars["volume"] = (lambda v=volume: int(round(float(v.get()))),
                               lambda x, v=volume: (v.set(x), volume_label.configure(text="%d%%" % x)))
        ttk.Label(tab, text="The stick, rumble, gamepads and key bindings: the Controls tab.",
                  style="Sub.TLabel").grid(row=2, column=2, columnspan=2, sticky="w", pady=3)

        section(4, 2, "Online")
        entry(5, 2, "Your name", "name", kind="name")
        self.build_server_row(tab, 6, 2, values["server"])
        entry(8, 2, "Direct-match port (UDP)", "p2p_port", width=8, kind=int)
        check(9, 2, "Watch King of the Hill fights while waiting", "watch_hill")
        check(10, 2, "Offline mode: never contact the server (direct matches still work)", "offline")

        ttk.Label(tab, text="These are saved to dojo.ini beside the game. The in-game options menu (F1) "
                  "changes the same settings.", style="Sub.TLabel", wraplength=640).grid(
            row=11, column=0, columnspan=4, sticky="w", pady=(18, 0))
        ttk.Button(tab, text="Reset to defaults", command=self.reset_settings).grid(
            row=12, column=0, sticky="w", pady=(8, 0))

    def build_server_row(self, tab, row, col, value):
        """The matchmaking server: the official one, locked, unless the player confirms they want another."""
        official = DEFAULTS["server"]
        ttk.Label(tab, text="Matchmaking server").grid(row=row, column=col, sticky="w", padx=(0, 8), pady=3)
        frame = ttk.Frame(tab)
        frame.grid(row=row, column=col + 1, sticky="w", padx=(0, 24))
        var = tk.StringVar(value=value)
        box = ttk.Entry(frame, textvariable=var, width=30, state="readonly")
        box.pack(side="left")
        button = ttk.Button(frame, width=10)
        button.pack(side="left", padx=(6, 0))
        note = ttk.Label(tab, text="", style="Warn.TLabel")
        note.grid(row=row + 1, column=col + 1, sticky="w")

        def lock():
            box.configure(state="readonly")
            custom = var.get().strip() != official
            button.configure(text="Official" if custom else "Change...", command=use_official if custom else change)
            note.configure(text="Not the official server" if custom else "")

        def use_official():
            var.set(official)
            lock()
            self.save_settings()

        def change():
            if not messagebox.askyesno(
                    "Dojo", "Use a different matchmaking server?\n\nThe official server (%s) is where everyone "
                    "else plays. Only change this to play on a server you or a friend runs; players on "
                    "different servers can't find each other." % official, icon="warning"):
                return
            box.configure(state="normal")
            box.focus_set()
            box.select_range(0, "end")
            button.configure(text="Done", command=done)

        def done():
            self.save_settings()
            var.set(self.vars["server"][0]())
            lock()

        box.bind("<Return>", lambda _: done() if str(box.cget("state")) == "normal" else None)
        box.bind("<FocusOut>", lambda _: self.save_settings())
        self.vars["server"] = (lambda: self.clean(var.get(), official, str),
                               lambda x: (var.set(str(x)), lock()))
        lock()

    @staticmethod
    def clean(text, default, kind):
        text = text.strip()
        if kind is int:
            try:
                return max(1, min(65535, int(text)))
            except ValueError:
                return default
        if kind == "name":
            text = "".join(c for c in text if c.isalnum() or c in "_-")[:23]
        else:
            text = text.split()[0] if text.split() else ""
        return text or default

    def settings(self):
        return {key: getter() for key, (getter, _) in self.vars.items()}

    def save_settings(self):
        """Writes what was changed here since the launcher last read dojo.ini, and nothing else: the game changes
        the file too (F1), so the launcher's copy of the other settings may be older than the file's."""
        try:
            current = self.settings()
            changed = {k: v for k, v in current.items() if v != self.shown_settings.get(k)}
            if not changed:
                return
            values = load_ini()
            values.update(changed)
            save_ini(values)
            self.shown_settings = current
            self.ini_stamp = ini_stamp()
        except (OSError, ValueError):
            pass

    def reload_settings(self):
        """Shows dojo.ini as it is now, if something else (the game) changed it since the launcher last looked."""
        if ini_stamp() == self.ini_stamp or getattr(self, "capturing", None) is not None:
            return
        values = load_ini()
        for key, (_, setter) in self.vars.items():
            setter(values[key])
        self.shown_settings = self.settings()
        self.ini_stamp = ini_stamp()
        self.controls = load_controls()
        self.refresh_pads()
        self.refresh_cells()

    def reset_settings(self):
        if not messagebox.askyesno("Dojo", "Put every setting back to its default?"):
            return
        for key, (_, setter) in self.vars.items():
            setter(DEFAULTS[key])
        save_ini(DEFAULTS)
        self.shown_settings = self.settings()
        self.ini_stamp = ini_stamp()

    # -- Build tab --

    def build_build_tab(self):
        tab = ttk.Frame(self.tabs, padding=16)
        self.tabs.add(tab, text="  Build  ")
        tab.columnconfigure(0, weight=1)
        tab.rowconfigure(4, weight=1)

        ttk.Label(tab, text="Build tools", style="Section.TLabel").grid(row=0, column=0, sticky="w")
        self.tool_frame = ttk.Frame(tab)
        self.tool_frame.grid(row=1, column=0, sticky="ew", pady=(4, 8))
        self.tool_frame.columnconfigure(1, weight=1)

        # The game's decompiled source: downloaded by Play, so a player never has to touch this.
        source = ttk.Frame(tab)
        source.grid(row=2, column=0, sticky="ew", pady=(0, 8))
        source.columnconfigure(1, weight=1)
        ttk.Label(source, text="Game source", width=12).grid(row=0, column=0, sticky="w")
        self.decomp_var = tk.StringVar(value=self.config["decomp"])
        ttk.Entry(source, textvariable=self.decomp_var).grid(row=0, column=1, sticky="ew")
        self.download_button = ttk.Button(source, text="Download", command=self.download_decomp)
        self.download_button.grid(row=0, column=2, padx=(6, 0))
        ttk.Button(source, text="Folder...", command=self.pick_decomp).grid(row=0, column=3, padx=(6, 0))
        self.decomp_info = ttk.Label(source, text="", wraplength=640)
        self.decomp_info.grid(row=1, column=1, columnspan=3, sticky="w", pady=(4, 0))
        self.decomp_var.trace_add("write", lambda *_: self.refresh_decomp())

        options = ttk.Frame(tab)
        options.grid(row=3, column=0, sticky="ew", pady=(0, 6))
        ttk.Label(options, text="Build type").pack(side="left")
        self.build_type = tk.StringVar(value=self.config["build_type"])
        box = ttk.Combobox(options, textvariable=self.build_type, state="readonly", width=16,
                           values=["Release", "RelWithDebInfo", "Debug"])
        box.pack(side="left", padx=6)
        box.bind("<<ComboboxSelected>>", lambda _: self.set_config("build_type", self.build_type.get()))
        ttk.Button(options, text="Check again", command=self.check_tools).pack(side="left", padx=6)
        ttk.Button(options, text="Open game log", command=self.open_game_log).pack(side="right")

        frame = ttk.Frame(tab)
        frame.grid(row=4, column=0, sticky="nsew")
        frame.columnconfigure(0, weight=1)
        frame.rowconfigure(0, weight=1)
        self.log = tk.Text(frame, height=12, wrap="none", font=("Consolas", 9), background="#0e0c0a",
                           foreground="#ddd", insertbackground="#ddd", state="disabled")
        self.log.grid(row=0, column=0, sticky="nsew")
        scroll = ttk.Scrollbar(frame, command=self.log.yview)
        scroll.grid(row=0, column=1, sticky="ns")
        self.log.configure(yscrollcommand=scroll.set)
        self.log.tag_configure("error", foreground="#ff7b72")
        self.log.tag_configure("good", foreground="#7ee787")

    def check_tools(self):
        for child in self.tool_frame.winfo_children():
            child.destroy()
        version = toolchain_version()
        if version:
            ttk.Label(self.tool_frame, text="OK", style="Good.TLabel", width=9).grid(row=0, column=0, sticky="w")
            ttk.Label(self.tool_frame, text="Bundled toolchain %s: Clang (llvm-mingw), CMake, Ninja, Python, SDL3, "
                      "glslang. Nothing to install." % version).grid(row=0, column=1, sticky="w")
        else:
            ttk.Label(self.tool_frame, text="Missing", style="Bad.TLabel", width=9).grid(row=0, column=0,
                                                                                        sticky="w")
            fetch = os.path.join(PORT_DIR, "tools", "fetch_toolchain.py")
            if getattr(sys, "frozen", False) or not os.path.isfile(fetch):
                ttk.Label(self.tool_frame, text="The toolchain folder is missing: unpack the whole Dojo zip again."
                          ).grid(row=0, column=1, sticky="w")
            else:
                ttk.Label(self.tool_frame, text="No toolchain in this checkout yet (about 200 MB to download)."
                          ).grid(row=0, column=1, sticky="w")
                ttk.Button(self.tool_frame, text="Fetch toolchain", command=self.fetch_toolchain).grid(
                    row=0, column=2, sticky="e")
        self.update_buttons()

    def fetch_toolchain(self):
        if self.busy:
            return
        python = sys.executable.replace("pythonw.exe", "python.exe")
        self.run_steps([[python, "-u", os.path.join(PORT_DIR, "tools", "fetch_toolchain.py")]], "Toolchain ready.",
                       after=self.check_tools)

    def copy(self, text):
        self.root.clipboard_clear()
        self.root.clipboard_append(text)
        self.status.configure(text="Copied: %s  (paste it into a terminal)" % text)

    def write_log(self, text, tag=None):
        self.log.configure(state="normal")
        self.log.insert("end", text, tag)
        self.log.see("end")
        self.log.configure(state="disabled")

    def open_game_log(self):
        if os.path.isfile(GAME_LOG):
            os.startfile(GAME_LOG)
        else:
            self.status.configure(text="No game log yet: it's written while the game runs.")

    def build(self, then_play=False):
        if self.busy or self.game_running():
            return
        problems = []
        if not self.decomp_ok():
            problems.append("Choose the decompilation folder (Game tab).")
        if not self.disc_bin():
            problems.append("Choose your disc image (Game tab).")
        if not toolchain_version():
            problems.append("The build tools are missing (Build tab).")
        if problems:
            messagebox.showwarning("Dojo", "Can't build yet:\n\n" + "\n".join(problems))
            return
        self.busy = "build"
        self.update_buttons()
        self.tabs.select(2)
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")

        configure = [CMAKE, "-S", PORT_DIR, "-B", BUILD_DIR, "-G", "Ninja",
                     "-DCMAKE_BUILD_TYPE=" + self.build_type.get(),
                     "-DDOJO_DECOMP_DIR=" + os.path.abspath(self.decomp_var.get().strip().strip('"')),
                     "-DDOJO_DISC=" + os.path.abspath(self.disc_bin())]
        cache = os.path.join(BUILD_DIR, "CMakeCache.txt")
        if os.path.isfile(cache):
            # A cache from before the bundled toolchain, or from a configure that failed to find the compiler
            # (no compiler flags, so no optimisation), is started over.
            with open(cache, encoding="utf-8", errors="replace") as f:
                text = f.read()
            if "CMAKE_C_FLAGS_%s:STRING=\n" % self.build_type.get().upper() in text or \
                    "i686-w64-mingw32-clang" not in text:
                os.remove(cache)
                shutil.rmtree(os.path.join(BUILD_DIR, "CMakeFiles"), ignore_errors=True)
        self.run_steps([configure, [CMAKE, "--build", BUILD_DIR]], "Built.", then_play=then_play)

    def run_steps(self, steps, success, then_play=False, after=None):
        """Runs the commands one after another, their output in the log; stops at the first that fails."""
        self.busy = "build"
        self.update_buttons()
        self.tabs.select(2)
        env = dict(os.environ)
        # Only the bundled tools: nothing installed on this PC takes part.
        env["PATH"] = os.pathsep.join([os.path.join(TOOLCHAIN, "llvm-mingw", "bin"), os.path.join(TOOLCHAIN, "ninja"),
                                       os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32")])
        self.after_steps = after

        def work():
            for step in steps:
                self.events.put(("log", "> " + " ".join('"%s"' % a if " " in a else a for a in step) + "\n"))
                try:
                    process = subprocess.Popen(step, cwd=PORT_DIR, env=env, stdout=subprocess.PIPE,
                                               stderr=subprocess.STDOUT, text=True, errors="replace",
                                               creationflags=NO_WINDOW)
                except OSError as e:
                    self.events.put(("build-done", (False, str(e), then_play)))
                    return
                for line in process.stdout:
                    self.events.put(("log", line))
                if process.wait() != 0:
                    self.events.put(("build-done", (False, "That failed: see the log above.", then_play)))
                    return
            self.events.put(("build-done", (True, success, then_play)))

        threading.Thread(target=work, daemon=True).start()

    # -- Controls tab --
    # The same settings as the game's controls page (F1, Controls; port/src/input.c): each player's gamepad, the
    # keyboard's player, the stick, rumble, and two gamepad and two keyboard-and-mouse bindings for each button.
    # The launcher sees the gamepads itself (Gamepads); a game built by this version also tests rumble the game's
    # way (dojo.exe --test-rumble, which knows a DualSense's haptics).

    def build_controls_tab(self):
        outer = ttk.Frame(self.tabs, padding=(16, 12, 4, 12))
        self.tabs.add(outer, text="  Controls  ")
        canvas = tk.Canvas(outer, background=PANEL, highlightthickness=0)
        scroll = ttk.Scrollbar(outer, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        canvas.pack(side="left", fill="both", expand=True)
        tab = ttk.Frame(canvas)
        canvas.create_window(0, 0, window=tab, anchor="nw")
        tab.bind("<Configure>", lambda _: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.bind("<Enter>", lambda _: canvas.bind_all(
            "<MouseWheel>", lambda e: canvas.yview_scroll(-e.delta // 120, "units")))
        canvas.bind("<Leave>", lambda _: canvas.unbind_all("<MouseWheel>"))

        values = load_ini()
        self.controls = load_controls()
        self.pads = []                # (guid, type, wired, name) from dojo.exe --pads
        self.capturing = None         # (button, slot) waiting for its input
        self.cells = {}

        # The devices, the stick and rumble.
        devices = ttk.Frame(tab)
        devices.grid(row=0, column=0, sticky="w")
        ttk.Label(devices, text="Devices", style="Section.TLabel").grid(row=0, column=0, columnspan=3, sticky="w")
        self.pad_boxes = []
        for player in range(2):
            ttk.Label(devices, text="Player %d gamepad" % (player + 1)).grid(row=1 + player, column=0, sticky="w",
                                                                           padx=(0, 8), pady=3)
            var = tk.StringVar()
            box = ttk.Combobox(devices, textvariable=var, state="readonly", width=40)
            box.grid(row=1 + player, column=1, sticky="w")
            box.bind("<<ComboboxSelected>>", lambda _, p=player: self.choose_pad(p))
            ttk.Button(devices, text="Test rumble", command=lambda p=player: self.test_rumble(p)).grid(
                row=1 + player, column=2, sticky="w", padx=(8, 0))
            self.pad_boxes.append((var, box))
        ttk.Label(devices, text="Keyboard & mouse").grid(row=3, column=0, sticky="w", padx=(0, 8), pady=3)
        self.combo_var(devices, 3, "keyboard_player", ["Player 1", "Player 2"], [1, 2], values)
        self.pad_note = ttk.Label(devices, text="", style="Sub.TLabel")
        self.pad_note.grid(row=4, column=1, columnspan=2, sticky="w")
        ttk.Button(devices, text="Find gamepads", command=self.find_pads).grid(row=4, column=0, sticky="w",
                                                                               pady=(2, 0))

        feel = ttk.Frame(tab)
        feel.grid(row=1, column=0, sticky="w", pady=(10, 0))
        ttk.Label(feel, text="Stick and rumble", style="Section.TLabel").grid(row=0, column=0, columnspan=4,
                                                                               sticky="w")
        var = tk.BooleanVar(value=bool(values["analog_stick"]))
        ttk.Checkbutton(feel, text="Left stick works the d-pad", variable=var, command=self.save_settings).grid(
            row=1, column=0, columnspan=2, sticky="w", pady=3)
        self.vars["analog_stick"] = (lambda v=var: int(v.get()), lambda x, v=var: v.set(bool(x)))
        ttk.Label(feel, text="Stick dead zone").grid(row=2, column=0, sticky="w", padx=(0, 8), pady=3)
        self.combo_var(feel, 2, "stick_deadzone", ["%d%%" % v for v in range(10, 100, 10)], list(range(10, 100, 10)),
                       values)
        ttk.Label(feel, text="Rumble").grid(row=1, column=2, sticky="w", padx=(32, 8), pady=3)
        self.combo_var(feel, 1, "rumble", ["Off"] + ["%d%%" % v for v in range(10, 110, 10)],
                       list(range(0, 110, 10)), values, column=3)
        var = tk.BooleanVar(value=bool(values["dualsense_haptics"]))
        ttk.Checkbutton(feel, text="DualSense haptics (on a USB cable)", variable=var,
                        command=self.save_settings).grid(row=2, column=2, columnspan=2, sticky="w", padx=(32, 0))
        self.vars["dualsense_haptics"] = (lambda v=var: int(v.get()), lambda x, v=var: v.set(bool(x)))
        self.rumble_note = ttk.Label(feel, text="", style="Sub.TLabel")
        self.rumble_note.grid(row=3, column=0, columnspan=4, sticky="w")

        # The bindings.
        table = ttk.Frame(tab)
        table.grid(row=2, column=0, sticky="w", pady=(10, 0))
        ttk.Label(table, text="Bindings", style="Section.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(table, text="Gamepad", style="Sub.TLabel").grid(row=0, column=1, columnspan=2)
        ttk.Label(table, text="Keyboard & mouse", style="Sub.TLabel").grid(row=0, column=3, columnspan=2)
        for c, text in enumerate(["Button", "Input 1", "Input 2", "Input 1", "Input 2"]):
            ttk.Label(table, text=text, style="Sub.TLabel").grid(row=1, column=c, sticky="w" if c == 0 else "")
        for b, (name, key) in enumerate(BUTTONS):
            ttk.Label(table, text=name).grid(row=2 + b, column=0, sticky="w", padx=(0, 16))
            for slot in range(4):
                cell = tk.Label(table, width=14, background=FIELD, foreground=PAPER, font=("Segoe UI", 10),
                                padx=4, pady=3, cursor="hand2")
                cell.grid(row=2 + b, column=1 + slot, padx=2, pady=2)
                cell.bind("<Button-1>", lambda e, b=b, s=slot: self.cell_clicked(e, b, s))
                cell.bind("<Button-3>", lambda _, b=b, s=slot: self.clear_cell(b, s))
                cell.bind("<Enter>", lambda _, c=cell: c.configure(background=FIELD_HOVER)
                          if c.cget("background") == FIELD else None)
                cell.bind("<Leave>", lambda _, c=cell: c.configure(background=FIELD)
                          if c.cget("background") == FIELD_HOVER else None)
                self.cells[(b, slot)] = cell
            ttk.Button(table, text="Reset", width=6, command=lambda b=b: self.reset_bindings(b)).grid(
                row=2 + b, column=5, padx=(6, 0))
        foot = ttk.Frame(tab)
        foot.grid(row=3, column=0, sticky="w", pady=(10, 0))
        ttk.Button(foot, text="Reset all bindings", command=lambda: self.reset_bindings(None)).pack(side="left")
        ttk.Label(foot, text="Click a cell, then press the key, mouse button or gamepad button for it. Right-click "
                  "clears it.", style="Sub.TLabel", wraplength=480).pack(side="left", padx=(12, 0))
        ttk.Label(tab, text="Also in the game: F1, Controls. In the game, F1 or Esc opens the menu; holding Backspace "
                  "(or the gamepad's back button) for 2 seconds goes back; F11 switches full screen.",
                  style="Sub.TLabel", wraplength=640).grid(row=4, column=0, sticky="w", pady=(12, 0))

        tab.bind_all("<KeyPress>", self.key_pressed, add="+")
        self.gamepads = Gamepads()
        self.controls_tab = outer
        self.refresh_pads()
        self.refresh_cells()
        self.find_pads()
        self.root.after(1500, self.watch_pads)

    def combo_var(self, parent, row, key, names, choices, values, column=1):
        var = tk.StringVar()
        current = values[key]
        var.set(names[choices.index(current)] if current in choices else names[-1])
        box = ttk.Combobox(parent, textvariable=var, values=names, state="readonly", width=12)
        box.grid(row=row, column=column, sticky="w")
        box.bind("<<ComboboxSelected>>", lambda _: self.save_settings())
        self.vars[key] = (lambda v=var: choices[names.index(v.get())],
                          lambda x, v=var: v.set(names[choices.index(x)] if x in choices else names[-1]))

    def pad_type(self):
        """The gamepad the names follow: player one's, or the first connected."""
        for player in range(2):
            guid = self.controls["pad%d" % (player + 1)]
            for pad in self.pads:
                if pad[0].lower() == guid.lower():
                    return pad[1]
        return self.pads[0][1] if self.pads else "xboxone"

    def refresh_cells(self):
        kind = self.pad_type()
        for (b, slot), cell in self.cells.items():
            binding = self.controls["binds"][b][slot]
            if self.capturing == (b, slot):
                cell.configure(text="Press a gamepad button..." if slot < 2 else "Press a key or click...",
                               background=CRIMSON, foreground="#ffffff")
            else:
                cell.configure(text=binding_name(binding, kind), background=FIELD,
                               foreground=PAPER if binding else DIM)

    def refresh_pads(self):
        choices = ["Auto (the first free gamepad)", "None"] + ["%s%s" % (p[3], "  (USB)" if p[2] else "")
                                                             for p in self.pads]
        for player, (var, box) in enumerate(self.pad_boxes):
            chosen = self.controls["pad%d" % (player + 1)]
            shown = list(choices)
            if chosen == "auto":
                var.set(shown[0])
            elif chosen == "none":
                var.set(shown[1])
            else:
                index = next((i for i, p in enumerate(self.pads) if p[0].lower() == chosen.lower()), None)
                if index is None:
                    shown.append("%s (not connected)" % (self.controls["pad%d_name" % (player + 1)] or "Gamepad"))
                    var.set(shown[-1])
                else:
                    var.set(shown[2 + index])
            box.configure(values=shown)

    def choose_pad(self, player):
        var, box = self.pad_boxes[player]
        index = list(box.cget("values")).index(var.get())
        key = "pad%d" % (player + 1)
        if index == 0:
            self.controls[key], self.controls[key + "_name"] = "auto", ""
        elif index == 1:
            self.controls[key], self.controls[key + "_name"] = "none", ""
        elif index - 2 < len(self.pads):
            self.controls[key], self.controls[key + "_name"] = self.pads[index - 2][0], self.pads[index - 2][3]
        save_controls(self.controls)
        self.refresh_pads()
        self.refresh_cells()

    def game_built(self):
        """dojo.exe is there and is this version's (an older one doesn't know the helper commands)."""
        return os.path.isfile(EXE) and self.config.get("built_version") == DOJO_VERSION

    def run_helper(self, args, done):
        """Runs dojo.exe with a helper command in a thread; done(output or None) back on this thread."""
        if not self.game_built():
            done(None)
            return

        def work():
            try:
                out = subprocess.run([EXE] + args, cwd=BUILD_DIR, capture_output=True, text=True, timeout=20,
                                     creationflags=NO_WINDOW).stdout
            except (OSError, subprocess.SubprocessError):
                out = None
            self.events.put(("call", lambda: done(out)))

        threading.Thread(target=work, daemon=True).start()

    def show_pads(self, pads):
        if pads != self.pads:
            self.pads = pads
            self.refresh_pads()
            self.refresh_cells()
        self.pad_note.configure(text="%d gamepad%s connected." % (len(pads), "" if len(pads) == 1 else "s")
                                if pads else "No gamepads connected.")

    def find_pads(self):
        if self.gamepads.available():
            if not self.game_running():
                self.show_pads(self.gamepads.list())
            return

        def done(out):
            if out is None:
                self.pad_note.configure(text="Can't see gamepads until the game is built (press Play once).")
                return
            self.show_pads([(p[1], p[2], p[3] == "1", p[4]) for p in (line.split(" ", 4) for line in out.splitlines())
                            if p[0] == "pad" and len(p) == 5])

        self.pad_note.configure(text="Looking for gamepads...")
        self.run_helper(["--pads"], done)

    def watch_pads(self):
        """While the Controls tab shows, gamepads plugged in or out appear at once; otherwise, or while the game
        runs (it has them), the launcher lets go of them."""
        if self.gamepads.available():
            if self.tabs.select() == str(self.controls_tab) and not self.game_running():
                if self.capturing is None or self.capturing[1] >= 2:
                    self.show_pads(self.gamepads.list())
            elif self.gamepads.open:
                self.gamepads.close()
        self.root.after(1000, self.watch_pads)

    def player_pad(self, player):
        """The index in self.pads of the gamepad a player gets, chosen as the game chooses (input.c)."""
        taken, chosen = set(), [None, None]
        for p in range(2):
            want = self.controls["pad%d" % (p + 1)].lower()
            if want not in ("auto", "none"):
                chosen[p] = next((i for i, pad in enumerate(self.pads) if pad[0].lower() == want and i not in taken),
                                 None)
                taken.add(chosen[p])
        for p in range(2):
            if self.controls["pad%d" % (p + 1)] == "auto":
                chosen[p] = next((i for i in range(len(self.pads)) if i not in taken), None)
                taken.add(chosen[p])
        return chosen[player]

    def test_rumble(self, player):
        if self.game_running():
            self.rumble_note.configure(text="The game is running: close it to test here.")
            return
        self.save_settings()
        index = self.player_pad(player)
        if index is None:
            self.rumble_note.configure(text="Player %d has no gamepad." % (player + 1))
            return
        if not self.game_built():
            # The launcher's own rumble: the motors (a DualSense's emulated ones), not its haptics.
            ok = self.gamepads.available() and self.gamepads.rumble(index, 0.6)
            self.rumble_note.configure(text="Player %d: that was the gamepad's rumble.%s" % (
                player + 1, " The game uses a wired DualSense's haptics: build it (Play) to test those here."
                if self.pads[index][1] == "ps5" and self.pads[index][2] else "")
                if ok else "Couldn't rumble player %d's gamepad." % (player + 1))
            return
        self.gamepads.close()   # the game's helper takes the gamepad for a moment

        def done(out):
            out = (out or "").strip()
            self.rumble_note.configure(text={
                "haptics": "Player %d: that was the DualSense's haptics." % (player + 1),
                "rumble": "Player %d: that was the gamepad's rumble." % (player + 1),
                "no gamepad": "Player %d has no gamepad." % (player + 1),
            }.get(out, "Couldn't test the rumble."))

        self.rumble_note.configure(text="Rumbling player %d's gamepad..." % (player + 1))
        self.run_helper(["--test-rumble", str(player + 1)], done)

    def set_binding(self, button, slot, binding):
        self.capturing = None
        self.controls["binds"][button][slot] = binding
        save_controls(self.controls)
        self.refresh_cells()

    def reset_bindings(self, button):
        if button is None and not messagebox.askyesno("Dojo", "Put every binding back to its default?"):
            return
        for b in range(len(BUTTONS)):
            if button is None or b == button:
                self.controls["binds"][b] = list(DEFAULT_BINDS[b])
        save_controls(self.controls)
        self.refresh_cells()

    def clear_cell(self, button, slot):
        if self.capturing is None or self.capturing[1] < 2:
            self.set_binding(button, slot, 0)   # (while a keyboard cell waits, a right-click is its binding)

    def cell_clicked(self, event, button, slot):
        if self.capturing is not None and self.capturing[1] >= 2:
            return   # a keyboard cell is waiting: this click is its binding (mouse_pressed)
        self.capturing = (button, slot)
        self.refresh_cells()
        if slot >= 2:
            # the next key, or a click anywhere in the window
            self.root.after(150, lambda: self.root.bind_all("<ButtonPress>", self.mouse_pressed, add="+"))
            return
        if self.game_running():
            self.capturing = None
            self.refresh_cells()
            self.rumble_note.configure(text="The game is running: change gamepad bindings in it (F1, Controls).")
            return
        if self.gamepads.available():
            self.gamepads.list()
            self.watch_capture(button, slot, self.gamepads.state(), 0)
            return

        def done(out):
            if self.capturing != (button, slot):
                return
            line = (out or "").strip()
            if line.startswith("bind "):
                self.set_binding(button, slot, int(line[5:], 16))
            else:
                self.capturing = None
                self.refresh_cells()
                if out is None:
                    self.rumble_note.configure(text="Build the game first: it reads the gamepad.")

        self.run_helper(["--capture-pad"], done)

    def watch_capture(self, button, slot, before, ticks):
        """Waits, ten seconds at most, for a gamepad press to bind."""
        if self.capturing != (button, slot):
            return
        now = self.gamepads.state()
        binding = Gamepads.pressed(before, now)
        if binding:
            self.set_binding(button, slot, binding)
        elif ticks > 600:
            self.capturing = None
            self.refresh_cells()
        else:
            self.root.after(16, lambda: self.watch_capture(button, slot, now, ticks + 1))

    def mouse_pressed(self, event):
        self.root.unbind_all("<ButtonPress>")
        if self.capturing is None or self.capturing[1] < 2:
            return
        if event.num in MOUSE_BUTTONS:
            self.set_binding(self.capturing[0], self.capturing[1], MOUSE | MOUSE_BUTTONS[event.num])

    def key_pressed(self, event):
        if self.capturing is None or self.capturing[1] < 2:
            return
        button, slot = self.capturing
        self.root.unbind_all("<ButtonPress>")
        if event.keysym == "Escape":
            self.capturing = None
            self.refresh_cells()
            return "break"
        if event.keysym == "Delete":
            self.set_binding(button, slot, 0)
            return "break"
        code = scancode_for(event)
        if code:
            self.set_binding(button, slot, KEY | code)
        return "break"

    # -- updates --

    def start_update_check(self):
        if not UPDATES:
            return
        shutil.rmtree(UPDATE_STAGE, ignore_errors=True)   # a staged update the script didn't get to
        if os.path.isfile(UPDATE_FAILED):
            # The last update couldn't put its files in place (a file in use?): offer it, don't force it again.
            os.remove(UPDATE_FAILED)
            self.update_blocked = True
            self.status.configure(text="The last update couldn't be installed (see update.log); "
                                  "Update now tries again.")

        def work():
            try:
                with urllib.request.urlopen(UPDATE_MANIFEST, timeout=15) as response:
                    manifest = json.loads(response.read().decode("utf-8"))
                if version_key(manifest["version"]) > version_key(DOJO_VERSION):
                    self.events.put(("update-found", manifest))
            except Exception:   # offline, or no manifest yet: no update, and nothing to tell the player
                pass

        threading.Thread(target=work, daemon=True).start()

    def show_banner(self, text):
        self.banner_text.configure(text=text)
        if not self.banner.winfo_ismapped():
            self.banner.pack(fill="x", padx=20, pady=(8, 0), before=self.tabs)

    def open_release_notes(self):
        if self.update and self.update.get("notes"):
            os.startfile(self.update["notes"])

    def update_now(self):
        self.update_wanted = True
        if self.update_state in (None, "failed"):
            self.download_update()
        else:
            self.maybe_install_update()

    def download_update(self):
        """Fetches the update (the whole release when the toolchain changed), checks it and unpacks it to
        update.new."""
        manifest = self.update
        package = manifest["update"] if manifest.get("toolchain") == toolchain_version() else manifest["full"]
        self.update_state = "downloading"
        self.update_button.configure(state="disabled")
        self.show_banner("Downloading Dojo %s..." % manifest["version"])

        def work():
            part = os.path.join(PORT_DIR, "update.zip.part")
            try:
                digest = hashlib.sha256()
                with urllib.request.urlopen(package["url"], timeout=30) as response, open(part, "wb") as out:
                    done = 0
                    while True:
                        chunk = response.read(1 << 18)
                        if not chunk:
                            break
                        out.write(chunk)
                        digest.update(chunk)
                        done += len(chunk)
                        self.events.put(("update-progress", (done, package.get("size", 0))))
                if digest.hexdigest() != package["sha256"]:
                    raise OSError("the download is damaged (its SHA-256 doesn't match)")
                shutil.rmtree(UPDATE_STAGE, ignore_errors=True)
                extract(part, UPDATE_STAGE, package.get("prefix", ""))
                if not os.path.isfile(os.path.join(UPDATE_STAGE, "Dojo.exe")):
                    raise OSError("the download has no Dojo.exe in it")
                self.events.put(("update-ready", None))
            except Exception as e:   # network, disk or a bad archive: all reported the same way
                shutil.rmtree(UPDATE_STAGE, ignore_errors=True)
                self.events.put(("update-ready", str(e)))
            finally:
                if os.path.exists(part):
                    os.remove(part)

        threading.Thread(target=work, daemon=True).start()

    def maybe_install_update(self):
        """Installs a staged update the player wants once nothing is building or playing."""
        if self.update_state != "ready" or not self.update_wanted:
            return
        if self.busy or self.game_running():
            self.show_banner("Dojo %s is ready: it installs when the game closes." % self.update["version"])
            return
        self.show_banner("Installing Dojo %s: the launcher closes and opens again..." % self.update["version"])
        self.root.after(1200, self.install_update)

    def install_update(self, relaunch=True):
        """Hands over to UPDATE_SCRIPT and closes; it opens the new launcher when it's done."""
        if not getattr(sys, "frozen", False):
            # A checkout run with DOJO_UPDATE_URL: everything up to here is tested, but the files aren't replaced.
            self.status.configure(text="Update staged in %s (a checkout isn't updated in place)." % UPDATE_STAGE)
            self.update_state = None
            return
        script = os.path.join(tempfile.gettempdir(), "dojo-update.cmd")
        # PyInstaller's variables would make the new Dojo.exe look for the old one's (deleted) unpacked files.
        env = {k: v for k, v in os.environ.items()
               if not k.upper().startswith(("_PYI", "_MEI")) and k.upper() not in ("TCL_LIBRARY", "TK_LIBRARY")}
        env.update(DOJO_PIDS="%d %d" % (os.getpid(), os.getppid()), DOJO_STAGE=UPDATE_STAGE, DOJO_HOME=PORT_DIR,
                   DOJO_RELAUNCH="1" if relaunch else "0", PYINSTALLER_RESET_ENVIRONMENT="1")
        try:
            with open(script, "w", encoding="ascii", newline="\r\n") as f:
                f.write(UPDATE_SCRIPT)
            with open(UPDATE_LOG, "w", encoding="utf-8") as log:
                subprocess.Popen(["cmd.exe", "/d", "/c", script], cwd=tempfile.gettempdir(), env=env,
                                 stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, close_fds=True,
                                 creationflags=NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP)
        except OSError as e:
            self.update_state = "failed"
            self.show_banner("Couldn't install the update: %s" % e)
            self.update_button.configure(state="normal")
            return
        self.update_state = "installing"
        self.close()

    # -- playing --

    def game_running(self):
        return self.game is not None and self.game.poll() is None

    def play(self):
        if self.busy or self.game_running():
            return
        if not self.disc_bin():
            messagebox.showwarning("Dojo", "Choose your disc image first (Game tab).")
            self.tabs.select(0)
            return
        chosen = os.path.normcase(os.path.abspath(self.decomp_var.get().strip().strip('"') or DECOMP_DIR))
        if chosen == os.path.normcase(DECOMP_DIR) and decomp_commit(DECOMP_DIR) != DECOMP_COMMIT:
            self.download_decomp(then_play=True)
            return
        # An updated launcher rebuilds the game once, from its version's source, whatever the checkbox says.
        if self.build_first_var.get() or not os.path.isfile(EXE) or self.config["built_version"] != DOJO_VERSION:
            if self.decomp_ok() and toolchain_version():
                self.build(then_play=True)
                return
            if not os.path.isfile(EXE):
                self.build()   # explains what's missing
                return
        self.launch()

    def launch(self):
        self.save_settings()
        self.gamepads.close()   # the game's now
        if not os.path.isdir(SAVES):
            # Saves made before the launcher, beside the decompilation, come along (copied, not moved).
            old = os.path.join(self.decomp_var.get().strip().strip('"'), "saves")
            if os.path.isdir(old):
                try:
                    shutil.copytree(old, SAVES)
                except OSError:
                    pass
        try:
            log = open(GAME_LOG, "w", encoding="utf-8", errors="replace")
            self.game = subprocess.Popen([EXE, os.path.abspath(self.disc_bin())], cwd=PORT_DIR, stdout=log,
                                         stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
            log.close()
        except OSError as e:
            messagebox.showerror("Dojo", "Couldn't start the game: %s" % e)
            return
        self.status.configure(text="Playing. (F1 in the game opens its options.)")
        self.update_buttons()
        threading.Thread(target=self.watch_game, args=(self.game,), daemon=True).start()

    def watch_game(self, process):
        code = process.wait()
        self.events.put(("game-exit", code))

    # -- events from the worker threads --

    def poll(self):
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == "call":
                    value()
                elif kind == "log":
                    tag = "error" if ("error" in value.lower() and "warning" not in value.lower()) else None
                    self.write_log(value, tag)
                elif kind == "build-done":
                    ok, text, then_play = value
                    self.busy = None
                    if getattr(self, "after_steps", None):
                        self.after_steps, after = None, self.after_steps
                        after()
                    self.write_log("\n" + text + "\n", "good" if ok else "error")
                    self.status.configure(text=text)
                    self.update_buttons()
                    if ok and os.path.isfile(EXE):
                        self.set_config("built_version", DOJO_VERSION)
                    if ok and then_play:
                        self.launch()
                    else:
                        self.maybe_install_update()
                elif kind == "download-progress":
                    text = "Downloading the game's source... %.1f MB" % (value / 1e6)
                    self.decomp_info.configure(text=text, style="TLabel")
                    self.status.configure(text=text)
                elif kind == "download-done":
                    self.busy = None
                    if value:
                        self.decomp_info.configure(text="The download failed: %s" % value, style="Bad.TLabel")
                        self.status.configure(text="Couldn't download the game's source; check the internet "
                                              "connection and press Play again.")
                    else:
                        self.status.configure(text="Downloaded the game's source.")
                        if os.path.normcase(self.decomp_var.get().strip()) != os.path.normcase(DECOMP_DIR):
                            self.decomp_var.set(DECOMP_DIR)
                        else:
                            self.refresh_decomp()
                    self.update_buttons()
                    if not value and getattr(self, "download_then_play", False):
                        self.download_then_play = False
                        self.play()
                elif kind == "verify-progress":
                    self.verify_progress["value"] = value
                elif kind == "verify-done":
                    self.busy = None
                    self.verify_progress.grid_remove()
                    name = KNOWN_DISCS.get(value)
                    if name:
                        self.disc_info.configure(text="Verified: %s, a good copy." % name, style="Good.TLabel")
                    elif value.startswith("error"):
                        self.disc_info.configure(text=value, style="Bad.TLabel")
                    else:
                        self.disc_info.configure(
                            text="Not the known USA disc (SHA-256 %s...). It may be another version or a bad "
                            "rip; online play needs both players on the same disc." % value[:16],
                            style="Warn.TLabel")
                    self.update_buttons()
                elif kind == "game-exit":
                    self.reload_settings()   # what was changed in the game's menu
                    self.status.configure(text="The game closed." if value == 0 else
                                          "The game stopped with an error (code %d)." % value)
                    if value != 0:
                        try:
                            with open(GAME_LOG, encoding="utf-8", errors="replace") as f:
                                tail = f.readlines()[-25:]
                        except OSError:
                            tail = []
                        self.write_log("\n--- the game's last output ---\n" + "".join(tail), "error")
                        self.tabs.select(2)
                    self.update_buttons()
                    self.maybe_install_update()
                elif kind == "update-found":
                    self.update = value
                    self.show_banner("Dojo %s is out (you have %s)." % (value["version"], DOJO_VERSION))
                    if self.config["auto_update"] and not self.update_blocked:
                        self.update_now()
                elif kind == "update-progress":
                    done, total = value
                    self.show_banner("Downloading Dojo %s... %.1f%s MB" % (
                        self.update["version"], done / 1e6, " of %.1f" % (total / 1e6) if total else ""))
                elif kind == "update-ready":
                    if value:
                        self.update_state = "failed"
                        self.update_button.configure(state="normal", text="Try again")
                        self.show_banner("Couldn't download Dojo %s: %s" % (self.update["version"], value))
                    else:
                        self.update_state = "ready"
                        self.update_button.configure(state="normal", text="Install now")
                        self.show_banner("Dojo %s is downloaded." % self.update["version"])
                        self.maybe_install_update()
        except queue.Empty:
            pass
        self.root.after(50, self.poll)

    def update_buttons(self):
        running = self.game_running()
        busy = self.busy is not None or running
        self.build_button.configure(state="disabled" if busy else "normal")
        self.play_button.configure(state="disabled" if busy else "normal",
                                   text="Playing..." if running else ("Working..." if self.busy else "Play"))
        self.verify_button.configure(state="disabled" if busy or not self.disc_bin() else "normal")
        self.download_button.configure(state="disabled" if busy else "normal")
        if os.path.isfile(EXE):
            self.exe_info.configure(text="Game built: %s" % EXE, style="Good.TLabel")
        else:
            self.exe_info.configure(text="Not built yet: Play builds it first (a few minutes the first time).",
                                    style="Warn.TLabel")

    def close(self):
        self.save_settings()
        self.save_config()
        if self.update_state == "ready" and not self.game_running():
            self.install_update(relaunch=False)   # downloaded but not installed yet: installed on the way out
            if self.update_state == "installing":
                return   # install_update closed the window
        self.root.destroy()


def main():
    try:
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)   # sharp text on high-DPI screens
    except (AttributeError, OSError):
        pass
    root = tk.Tk()
    Launcher(root)
    root.mainloop()


if __name__ == "__main__":
    main()
