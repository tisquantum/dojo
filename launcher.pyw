"""Dojo launcher: point it at your disc image, set the options, build and play.

In a release it is Dojo.exe beside the source and the bundled toolchain (toolchain/); in a checkout, run
`python launcher.pyw` after `python tools/fetch_toolchain.py`. It downloads the decompilation the game is built
from (or uses one you choose), builds the game with the bundled tools, and starts it. It keeps its own choices
in launcher.json, writes the game's options to build/dojo.ini (the file the in-game options menu, F1, changes
too), and runs the game in this folder, so the memory-card saves live in saves/ here.
"""

import hashlib
import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import urllib.request
import zipfile
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

DOJO_VERSION = "0.1.0"

# The folder of Dojo.exe in a release, of this file in a checkout.
PORT_DIR = os.path.dirname(os.path.abspath(sys.executable if getattr(sys, "frozen", False) else __file__))
TOOLCHAIN = os.path.join(PORT_DIR, "toolchain")
CMAKE = os.path.join(TOOLCHAIN, "cmake", "bin", "cmake.exe")
DECOMP_DIR = os.path.join(PORT_DIR, "decomp")

# The decompilation, with Dojo's changes, this version is built from: a branch of a fork of the decompilation,
# fetched at this exact commit.
DECOMP_COMMIT = "276a3af20be7c51b0e7d72f6d90cbe6814419ad6"
DECOMP_URL = "https://github.com/tisquantum/bushido/archive/%s.zip" % DECOMP_COMMIT
DECOMP_STAMP = ".dojo-decomp"
BUILD_DIR = os.path.join(PORT_DIR, "build")
EXE = os.path.join(BUILD_DIR, "dojo.exe")
INI = os.path.join(BUILD_DIR, "dojo.ini")
LAUNCHER_JSON = os.path.join(PORT_DIR, "launcher.json")
GAME_LOG = os.path.join(PORT_DIR, "dojo.log")
SAVES = os.path.join(PORT_DIR, "saves")

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


def default_name():
    name = "".join(c for c in os.environ.get("USERNAME", "") if c.isalnum() or c in "_-")[:23]
    return name or "Samurai"


# The game's options, in the order and with the defaults of port/src/settings.c.
DEFAULTS = {
    "aspect": "auto", "name": default_name(), "server": "dojo-matchmaker.fly.dev:7777",
    "fps": 0, "scale": 4, "software": 0, "fullscreen": 0, "vsync": 0, "volume": 100, "show_fps": 0,
    "analog_stick": 0, "p2p_port": 7778, "watch_hill": 1, "offline": 0,
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


def save_ini(values):
    os.makedirs(BUILD_DIR, exist_ok=True)
    lines = ["; Dojo: the options menu (F1) writes this file.\n"]
    lines += ["%s = %s\n" % (key, values[key]) for key in DEFAULTS]
    with open(INI, "w", encoding="utf-8") as f:
        f.writelines(lines)


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
        root.minsize(720, 560)
        style = ttk.Style()
        if "vista" in style.theme_names():
            style.theme_use("vista")
        style.configure("Title.TLabel", font=("Segoe UI", 22, "bold"))
        style.configure("Sub.TLabel", foreground="#666")
        style.configure("Play.TButton", font=("Segoe UI", 11, "bold"), padding=(18, 6))
        style.configure("Good.TLabel", foreground="#1a7f37")
        style.configure("Bad.TLabel", foreground="#c0392b")
        style.configure("Warn.TLabel", foreground="#9a6700")

        header = ttk.Frame(root, padding=(16, 12, 16, 4))
        header.pack(fill="x")
        ttk.Label(header, text="DOJO", style="Title.TLabel").pack(side="left")
        ttk.Label(header, text="   Bushido Blade for PC: high frame rates, any screen shape, online play",
                  style="Sub.TLabel").pack(side="left", pady=(10, 0))

        self.tabs = ttk.Notebook(root)
        self.tabs.pack(fill="both", expand=True, padx=12, pady=6)
        self.build_game_tab()
        self.build_settings_tab()
        self.build_build_tab()
        self.build_controls_tab()

        bar = ttk.Frame(root, padding=(16, 6, 16, 12))
        bar.pack(fill="x")
        self.status = ttk.Label(bar, text="")
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
        root.after(50, self.poll)

    # -- the launcher's own settings --

    def load_config(self):
        config = {"disc": "", "decomp": "", "build_type": "Release", "build_first": True}
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

        ttk.Label(tab, text="Disc image", font=("Segoe UI", 10, "bold")).grid(row=0, column=0, sticky="w")
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

        ttk.Label(tab, text="Decompilation folder", font=("Segoe UI", 10, "bold")).grid(row=6, column=0,
                                                                                         sticky="w")
        ttk.Label(tab, text="The decompiled source of Bushido Blade, with Dojo's changes, that the game is built "
                  "from. It isn't shipped with Dojo: Download fetches this version's copy from GitHub.",
                  style="Sub.TLabel", wraplength=640).grid(row=7, column=0, columnspan=4, sticky="w", pady=(0, 6))
        self.decomp_var = tk.StringVar(value=self.config["decomp"])
        ttk.Entry(tab, textvariable=self.decomp_var).grid(row=8, column=0, columnspan=2, sticky="ew")
        self.download_button = ttk.Button(tab, text="Download", command=self.download_decomp)
        self.download_button.grid(row=8, column=2, padx=(6, 0))
        ttk.Button(tab, text="Folder...", command=self.pick_decomp).grid(row=8, column=3, padx=(6, 0))
        self.decomp_var.trace_add("write", lambda *_: self.refresh_decomp())
        self.decomp_info = ttk.Label(tab, text="")
        self.decomp_info.grid(row=9, column=0, columnspan=4, sticky="w", pady=(6, 0))

        ttk.Separator(tab).grid(row=10, column=0, columnspan=4, sticky="ew", pady=16)
        self.exe_info = ttk.Label(tab, text="")
        self.exe_info.grid(row=11, column=0, columnspan=4, sticky="w")
        self.build_first_var = tk.BooleanVar(value=self.config["build_first"])
        ttk.Checkbutton(tab, text="Build before playing (picks up changes; quick when nothing changed)",
                        variable=self.build_first_var,
                        command=lambda: self.set_config("build_first", self.build_first_var.get())
                        ).grid(row=12, column=0, columnspan=4, sticky="w", pady=(8, 0))

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
            self.decomp_info.configure(text="This is another version's decompilation: press Download for this "
                                       "version's.", style="Warn.TLabel")
        elif self.decomp_ok():
            self.decomp_info.configure(text="Ready." if commit else "Found the decompilation (your own copy).",
                                       style="Good.TLabel")
        elif path and os.path.isfile(os.path.join(path, "src", "main.c")):
            self.decomp_info.configure(text="This decompilation is missing the port's changes "
                                       "(include/substep.h).", style="Bad.TLabel")
        elif path and os.path.normcase(os.path.abspath(path)) == os.path.normcase(DECOMP_DIR) and \
                not os.path.isdir(path):
            self.decomp_info.configure(text="Not downloaded yet: press Download (a few MB).",
                                       style="Warn.TLabel")
        else:
            self.decomp_info.configure(text="Not the decompilation (no src/main.c in it).",
                                       style="Bad.TLabel" if path else "Warn.TLabel")
        if hasattr(self, "play_button"):
            self.update_buttons()

    def download_decomp(self):
        if self.busy:
            return
        dest = DECOMP_DIR
        if os.path.isdir(dest) and os.listdir(dest) and decomp_commit(dest) is None:
            messagebox.showwarning("Dojo", "%s holds files the launcher didn't download; move them away first."
                                   % dest)
            return
        self.busy = "download"
        self.update_buttons()
        self.decomp_info.configure(text="Downloading...", style="TLabel")

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
                top = "bushido-%s/" % DECOMP_COMMIT
                fresh = dest + ".new"
                shutil.rmtree(fresh, ignore_errors=True)
                with zipfile.ZipFile(part) as z:
                    for info in z.infolist():
                        if not info.filename.startswith(top) or info.filename.endswith("/"):
                            continue
                        target = os.path.normpath(os.path.join(fresh, info.filename[len(top):]))
                        if not target.startswith(os.path.normpath(fresh) + os.sep):
                            raise OSError("unsafe path in the download: " + info.filename)
                        os.makedirs(os.path.dirname(target), exist_ok=True)
                        with z.open(info) as src, open(target, "wb") as out:
                            shutil.copyfileobj(src, out)
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
            ttk.Label(tab, text=title, font=("Segoe UI", 10, "bold")).grid(
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
        combo(2, 0, "Screen shape", "aspect", ASPECT_NAMES, ASPECTS)
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
        check(2, 2, "Left stick works the d-pad", "analog_stick")

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
        try:
            values = load_ini()
            values.update(self.settings())
            save_ini(values)
        except (OSError, ValueError):
            pass

    def reset_settings(self):
        if not messagebox.askyesno("Dojo", "Put every setting back to its default?"):
            return
        for key, (_, setter) in self.vars.items():
            setter(DEFAULTS[key])
        save_ini(DEFAULTS)

    # -- Build tab --

    def build_build_tab(self):
        tab = ttk.Frame(self.tabs, padding=16)
        self.tabs.add(tab, text="  Build  ")
        tab.columnconfigure(0, weight=1)
        tab.rowconfigure(3, weight=1)

        ttk.Label(tab, text="Build tools", font=("Segoe UI", 10, "bold")).grid(row=0, column=0, sticky="w")
        self.tool_frame = ttk.Frame(tab)
        self.tool_frame.grid(row=1, column=0, sticky="ew", pady=(4, 8))
        self.tool_frame.columnconfigure(1, weight=1)

        options = ttk.Frame(tab)
        options.grid(row=2, column=0, sticky="ew", pady=(0, 6))
        ttk.Label(options, text="Build type").pack(side="left")
        self.build_type = tk.StringVar(value=self.config["build_type"])
        box = ttk.Combobox(options, textvariable=self.build_type, state="readonly", width=16,
                           values=["Release", "RelWithDebInfo", "Debug"])
        box.pack(side="left", padx=6)
        box.bind("<<ComboboxSelected>>", lambda _: self.set_config("build_type", self.build_type.get()))
        ttk.Button(options, text="Check again", command=self.check_tools).pack(side="left", padx=6)
        ttk.Button(options, text="Open game log", command=self.open_game_log).pack(side="right")

        frame = ttk.Frame(tab)
        frame.grid(row=3, column=0, sticky="nsew")
        frame.columnconfigure(0, weight=1)
        frame.rowconfigure(0, weight=1)
        self.log = tk.Text(frame, height=12, wrap="none", font=("Consolas", 9), background="#111",
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

    def build_controls_tab(self):
        tab = ttk.Frame(self.tabs, padding=16)
        self.tabs.add(tab, text="  Controls  ")
        rows = [
            ("PlayStation", "Keyboard", "Gamepad"),
            ("D-pad", "Arrow keys", "D-pad (or left stick, if switched on)"),
            ("Triangle", "S", "Y / Triangle"),
            ("Circle", "C", "B / Circle"),
            ("Cross", "X", "A / Cross"),
            ("Square", "Z", "X / Square"),
            ("L1 / R1", "Q / E", "Shoulder buttons"),
            ("L2 / R2", "1 / 3", "Triggers"),
            ("Start", "Enter", "Start"),
            ("Select", "Backspace", "Back / View"),
            ("", "", ""),
            ("Options menu", "F1 or Esc", "Guide button"),
            ("Back out", "Hold Backspace 2 s", "Hold Back 2 s"),
            ("Full screen", "F11", ""),
        ]
        for r, row in enumerate(rows):
            for c, text in enumerate(row):
                font = ("Segoe UI", 10, "bold") if r == 0 else None
                ttk.Label(tab, text=text, font=font).grid(row=r, column=c, sticky="w", padx=(0, 32), pady=2)

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
        if not self.decomp_ok() and os.path.normcase(os.path.abspath(self.decomp_var.get().strip().strip('"'))) \
                == os.path.normcase(DECOMP_DIR):
            if messagebox.askyesno("Dojo", "The game is built from its decompiled source, which isn't downloaded "
                                   "yet. Download it now (a few MB)?"):
                self.download_decomp()
            return
        if self.build_first_var.get() or not os.path.isfile(EXE):
            if self.decomp_ok() and toolchain_version():
                self.build(then_play=True)
                return
            if not os.path.isfile(EXE):
                self.build()   # explains what's missing
                return
        self.launch()

    def launch(self):
        self.save_settings()
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
                if kind == "log":
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
                    if ok and then_play:
                        self.launch()
                elif kind == "download-progress":
                    self.decomp_info.configure(text="Downloading... %.1f MB" % (value / 1e6), style="TLabel")
                elif kind == "download-done":
                    self.busy = None
                    if value:
                        self.decomp_info.configure(text="The download failed: %s" % value, style="Bad.TLabel")
                    elif os.path.normcase(self.decomp_var.get().strip()) != os.path.normcase(DECOMP_DIR):
                        self.decomp_var.set(DECOMP_DIR)
                    else:
                        self.refresh_decomp()
                    self.update_buttons()
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
