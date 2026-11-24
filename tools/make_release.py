#!/usr/bin/env python3
"""Make the release zip: dist/Dojo-<version>-windows.zip.

It holds Dojo.exe (the launcher, packaged with PyInstaller), this repository's files, the bundled toolchain
(tools/fetch_toolchain.py) and the game's decompiled source (decomp/, the commit the launcher pins, fetched
from the fork), so a player unpacks it, opens Dojo.exe, points it at their own disc and plays: nothing to
install or download. No game data is in it: the game is built on the player's machine from that source and
their disc.

    python tools/make_release.py            (needs PyInstaller: pip install pyinstaller)
"""

import os
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(HERE, "dist")

THIRD_PARTY = """# Third-party software in this release

Dojo's own code is in the folders beside this file. The `toolchain` folder holds the open-source tools Dojo is
built with, unmodified except that parts the build doesn't use were left out. Each keeps its licence:

| Folder | Software | Licence |
|---|---|---|
| `toolchain/llvm-mingw` | llvm-mingw (LLVM, Clang, LLD, mingw-w64) | Apache 2.0 with LLVM exceptions; mingw-w64's licences (see `LICENSE.TXT`) |
| `toolchain/cmake` | CMake | BSD 3-clause (`doc/cmake-3.31/Copyright.txt`) |
| `toolchain/ninja` | Ninja | Apache 2.0 (`COPYING`) |
| `toolchain/python` | Python (embeddable) | PSF licence (`LICENSE.txt`) |
| `toolchain/deps/SDL3` | SDL 3 | zlib (`LICENSE.txt`) |
| `toolchain/deps/Vulkan-Headers` | Vulkan headers | Apache 2.0 / MIT (`LICENSE.md`) |
| `toolchain/deps/glslang` | glslang | BSD and others (`LICENSE.txt`) |

`Dojo.exe` is the launcher packaged with PyInstaller, which includes Python and Tcl/Tk (PSF and BSD-style
licences).

`decomp` is the decompiled source of Bushido Blade with Dojo's changes (github.com/tisquantum/bushido, branch
`dojo`, a fork of github.com/virtualonno/bushido). It isn't covered by Dojo's MIT licence.

Bushido Blade is a trademark of its owners. No game data is included; you need your own copy of the game.
"""


def launcher_constant(name):
    text = open(os.path.join(HERE, "launcher.pyw"), encoding="utf-8").read()
    return re.search(r'%s = "([^"]+)"' % name, text).group(1)


def version():
    return launcher_constant("DOJO_VERSION")


def add_decomp(dest):
    """The decompiled source at the commit the launcher pins, from the fork, as decomp/ (stamped like a download)."""
    commit = launcher_constant("DECOMP_COMMIT")
    archive = os.path.join(DIST, "decomp-%s.zip" % commit)
    if not os.path.isfile(archive):
        url = "https://github.com/tisquantum/bushido/archive/%s.zip" % commit
        print("downloading", url, flush=True)
        with urllib.request.urlopen(url) as response, open(archive + ".part", "wb") as out:
            shutil.copyfileobj(response, out)
        os.replace(archive + ".part", archive)
    top = "bushido-%s/" % commit
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            if info.filename.startswith(top) and not info.filename.endswith("/"):
                target = os.path.join(dest, info.filename[len(top):])
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with z.open(info) as src, open(target, "wb") as out:
                    shutil.copyfileobj(src, out)
    with open(os.path.join(dest, ".dojo-decomp"), "w") as f:
        f.write(commit + "\n")


def repository_files():
    """The repository's files (tracked or new, not ignored)."""
    out = subprocess.run(["git", "ls-files", "--cached", "--others", "--exclude-standard"], cwd=HERE,
                         capture_output=True, text=True, check=True).stdout
    return [line for line in out.splitlines() if line and os.path.isfile(os.path.join(HERE, line))]


def main():
    ver = version()
    toolchain = os.path.join(HERE, "toolchain")
    if not os.path.isfile(os.path.join(toolchain, "TOOLCHAIN_VERSION")):
        raise SystemExit("no toolchain: run tools/fetch_toolchain.py first")
    name = "Dojo-%s" % ver
    stage = os.path.join(DIST, name)
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)

    print("packaging the launcher", flush=True)
    work = os.path.join(DIST, "_pyinstaller")
    subprocess.run([sys.executable, "-m", "PyInstaller", "--noconfirm", "--onefile", "--windowed", "--name", "Dojo",
                    "--distpath", stage, "--workpath", work, "--specpath", work,
                    os.path.join(HERE, "launcher.pyw")], check=True)

    print("copying the source", flush=True)
    for rel in repository_files():
        dest = os.path.join(stage, rel)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.copy2(os.path.join(HERE, rel), dest)

    print("copying the toolchain", flush=True)
    shutil.copytree(toolchain, os.path.join(stage, "toolchain"),
                    ignore=shutil.ignore_patterns("_downloads", "__pycache__"))
    print("adding the game's decompiled source", flush=True)
    add_decomp(os.path.join(stage, "decomp"))
    with open(os.path.join(stage, "THIRD_PARTY.md"), "w", encoding="utf-8") as f:
        f.write(THIRD_PARTY)

    archive = os.path.join(DIST, "%s-windows.zip" % name)
    print("zipping", archive, flush=True)
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for dirpath, _, files in os.walk(stage):
            for file in files:
                full = os.path.join(dirpath, file)
                z.write(full, os.path.join(name, os.path.relpath(full, stage)))
    print("done: %s (%.0f MB)" % (archive, os.path.getsize(archive) / 1e6))


if __name__ == "__main__":
    main()
