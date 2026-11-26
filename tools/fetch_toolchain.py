#!/usr/bin/env python3
"""Fetch the self-contained toolchain Dojo is built with into toolchain/ (or the folder given).

Every player builds the game with these exact tools, so every build is the same program (which online play
relies on), and nothing has to be installed. On Windows: llvm-mingw (Clang, the linker and the Windows headers
and libraries, for 32-bit x86 only), CMake, Ninja, the embeddable Python the build's scripts run on, SDL3, the
Vulkan headers and the glslang shader compiler. On Linux: Zig (Clang, the linker, and glibc's headers and
stubs for 32-bit x86), CMake, Ninja, Debian's 32-bit SDL3 (the same version as the Windows one), the Vulkan
headers and glslang; the build's scripts run on the system's own Python. Each download is checked against its
SHA-256. All of them are open source; their licences are in their folders.

    python3 tools/fetch_toolchain.py [toolchain folder] [--downloads folder]

The release ships the folder this makes, so players never run this.
"""

import hashlib
import io
import os
import shutil
import sys
import tarfile
import urllib.request
import zipfile

# name, url, sha256, folder inside the toolchain, the archive's own top folder to drop (or "")
PACKAGES = [
    ("llvm-mingw", "https://github.com/mstorsjo/llvm-mingw/releases/download/20261104/"
     "llvm-mingw-20261104-ucrt-x86_64.zip",
     "552191b5754c49a5a20723276a6fc56541ea1922c4c50b7b669022333fc1b40f", "llvm-mingw",
     "llvm-mingw-20261104-ucrt-x86_64"),
    ("CMake", "https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-windows-x86_64.zip",
     "d163cd3ab4959b0a53fa8988f2ddbd2e6c501658201e6a154386bad9dbe4f836", "cmake", "cmake-3.31.6-windows-x86_64"),
    ("Ninja", "https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip",
     "f550fec705b6d6ff58f2db3c374c2277a37691678d6aba463adcbb129108467a", "ninja", ""),
    ("Python", "https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip",
     "4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3", "python", ""),
    ("SDL3", "https://github.com/libsdl-org/SDL/releases/download/release-3.2.10/SDL3-devel-3.2.10-mingw.zip",
     "33bbcd16cdf3af5b0bac441bd922936c7b1336714471feda0147607a693d16aa", "deps/SDL3", "SDL3-3.2.10"),
    ("Vulkan headers", "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.341.0.zip",
     "1a0ea8ce1f1f08bcf9a7e4962646ba6b2a09b40108ca516fc6a006e45c77cb65", "deps/Vulkan-Headers",
     "Vulkan-Headers-vulkan-sdk-1.4.341.0"),
    ("glslang", "https://github.com/KhronosGroup/glslang/releases/download/16.5.0/"
     "glslang-16.5.0-windows-x86_64-release.zip",
     "06b71298b750268c127f2ee7ae0ef7525e2068120c6c8a3a08b2f58ca6f325ce", "deps/glslang", ""),
    # Licences the two archives above leave out.
    ("glslang licence", "https://raw.githubusercontent.com/KhronosGroup/glslang/16.5.0/LICENSE.txt",
     "17e70c676e1521ff3e4686f04a2053d93a7e28a33be8de7ec37ab0ff72feb677", "deps/glslang", ""),
    ("Ninja licence", "https://raw.githubusercontent.com/ninja-build/ninja/v1.12.1/COPYING",
     "eb7e9ab9690124c5c9f42bdc81383d886a3dede26345b6ed15bbad7caf81f7ea", "ninja", ""),
]

LINUX_PACKAGES = [
    ("Zig", "https://ziglang.org/download/0.14.1/zig-x86_64-linux-0.14.1.tar.xz",
     "24aeeec8af16c381934a6cd7d95c807a8cb2cf7df9fa40d359aa884195c4716c", "zig", "zig-x86_64-linux-0.14.1"),
    ("CMake", "https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-x86_64.tar.gz",
     "5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf", "cmake", "cmake-3.31.6-linux-x86_64"),
    ("Ninja", "https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-linux.zip",
     "6f98805688d19672bd699fbbfa2c2cf0fc054ac3df1f0e6a47664d963d530255", "ninja", ""),
    # Debian's packages of SDL 3.2.10 for 32-bit x86: the library, then its headers (deb_members says what
    # is taken from each).
    ("SDL3", "https://deb.debian.org/debian/pool/main/libs/libsdl3/libsdl3-0_3.2.10+ds-1_i386.deb",
     "79f9247f7510b013c6c1801aba7e831aa9b9b0f54d1ffd50b5402d9d17e0c06a", "deps/SDL3/i686-linux-gnu", ""),
    ("SDL3 headers", "https://deb.debian.org/debian/pool/main/libs/libsdl3/libsdl3-dev_3.2.10+ds-1_i386.deb",
     "f6b3a1f35b10c5f7f087a4a6b810f9d24b94e3011b1513f90ab54263f1f674db", "deps/SDL3/i686-linux-gnu", ""),
    PACKAGES[5],  # the Vulkan headers
    ("glslang", "https://github.com/KhronosGroup/glslang/releases/download/16.5.0/"
     "glslang-16.5.0-linux-x86_64-release.zip",
     "dfc3fb889eeb9344dce58606a0b37bde89f1eec6140f4e9b4b3d9305f46f64f4", "deps/glslang", ""),
    PACKAGES[7],  # glslang's licence
    PACKAGES[8],  # Ninja's
]

# Bumped when PACKAGES or the trimming changes; the launcher and the build check it.
TOOLCHAIN_VERSION = "2"
LINUX_TOOLCHAIN_VERSION = "linux-1"

# llvm-mingw targets four CPUs; the game needs one.
OTHER_TARGETS = ("aarch64", "armv7", "x86_64", "arm64ec")


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(url, path, expected):
    if os.path.isfile(path) and sha256(path) == expected:
        return
    print("downloading", url, flush=True)
    part = path + ".part"
    with urllib.request.urlopen(url) as response, open(part, "wb") as out:
        shutil.copyfileobj(response, out, 1 << 20)
    actual = sha256(part)
    if actual != expected:
        os.remove(part)
        raise SystemExit("%s: SHA-256 %s, expected %s" % (url, actual, expected))
    os.replace(part, path)


def safe_target(archive, dest, name):
    target = os.path.normpath(os.path.join(dest, name))
    if not target.startswith(os.path.normpath(dest) + os.sep):
        raise SystemExit("%s: unsafe path %s" % (archive, name))
    os.makedirs(os.path.dirname(target), exist_ok=True)
    return target


def extract_tar(archive, dest, top):
    """Unpacks a .tar.gz or .tar.xz into dest, dropping its top folder; files keep their permissions."""
    with tarfile.open(archive) as t:
        for info in t:
            name = info.name
            if top:
                if not name.startswith(top + "/"):
                    continue
                name = name[len(top) + 1:]
            if not name or info.isdir():
                continue
            target = safe_target(archive, dest, name)
            if info.issym():
                if os.path.isabs(info.linkname) or ".." in info.linkname.split("/"):
                    continue
                if os.path.lexists(target):
                    os.remove(target)
                os.symlink(info.linkname, target)
            elif info.isfile():
                with t.extractfile(info) as src, open(target, "wb") as out:
                    shutil.copyfileobj(src, out, 1 << 20)
                os.chmod(target, info.mode & 0o755 | 0o600)


def deb_data(archive):
    """A Debian package's files: the data.tar.* member of its ar archive, opened."""
    with open(archive, "rb") as f:
        if f.read(8) != b"!<arch>\n":
            raise SystemExit("%s: not a Debian package" % archive)
        while True:
            header = f.read(60)
            if len(header) < 60:
                raise SystemExit("%s: no data in the package" % archive)
            name = header[:16].decode().strip().rstrip("/")
            size = int(header[48:58])
            body = f.read(size)
            if size % 2:
                f.read(1)
            if name.startswith("data.tar"):
                return tarfile.open(fileobj=io.BytesIO(body))


def extract_deb(archive, dest):
    """From Debian's SDL3 packages: the library (as its soname, libSDL3.so.0), its copyright, the headers."""
    with deb_data(archive) as t:
        for info in t:
            name = info.name[2:] if info.name.startswith("./") else info.name
            if not info.isfile():
                continue
            if name.startswith("usr/lib/i386-linux-gnu/libSDL3.so.0."):
                out = "lib/libSDL3.so.0"
            elif name == "usr/share/doc/libsdl3-0/copyright":
                out = "LICENSE.txt"
            elif name.startswith("usr/include/SDL3/"):
                out = "include/" + name[len("usr/include/"):]
            else:
                continue
            target = safe_target(archive, dest, out)
            with t.extractfile(info) as src, open(target, "wb") as f:
                shutil.copyfileobj(src, f, 1 << 20)
            os.chmod(target, 0o755 if out.startswith("lib/") else 0o644)


def extract(archive, dest, top):
    """Unpacks archive into dest, dropping its top folder."""
    if archive.endswith((".tar.gz", ".tar.xz")):
        return extract_tar(archive, dest, top)
    if archive.endswith(".deb"):
        return extract_deb(archive, dest)
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            name = info.filename
            if top:
                if not name.startswith(top + "/"):
                    continue
                name = name[len(top) + 1:]
            if not name or name.endswith("/"):
                continue
            target = safe_target(archive, dest, name)
            with z.open(info) as src, open(target, "wb") as out:
                shutil.copyfileobj(src, out, 1 << 20)
            mode = info.external_attr >> 16
            if mode and os.name != "nt":
                os.chmod(target, mode & 0o755 | 0o600)


# What the build never uses, per package (paths inside its folder); the trimmed toolchain is about a third.
UNUSED = {
    "llvm-mingw": ["bin/clangd.exe", "bin/clang-tidy.exe", "bin/libpython3.14.dll", "bin/libpython3.dll",
                   "bin/libffi-8.dll", "bin/libclang_rt.asan_dynamic-x86_64.dll", "bin/libomp.dll",
                   "bin/widl.exe", "bin/i686-w64-mingw32-widl.exe", "bin/llvm-cov.exe", "bin/llvm-profdata.exe",
                   "bin/mingw32-make.exe", "bin/run-clang-tidy", "bin/scan-build-py", "bin/analyze-build",
                   "bin/intercept-build", "bin/git-clang-format", "lib/libscanbuild", "lib/libear", "share"],
    "CMake": ["doc", "man", "bin/cmake-gui.exe", "bin/cpack.exe", "bin/ctest.exe", "bin/cmcldeps.exe",
              "bin/cmake-gui", "bin/cpack", "bin/ctest", "bin/ccmake"],
    "Zig": ["doc"],
    "SDL3": ["x86_64-w64-mingw32", "cmake", "Makefile"],
    "Vulkan headers": ["registry", "tests", "BUILD.gn", "Makefile.release"],
    "glslang": ["lib", "include"],
}
KEEP = {"CMake": ["doc/cmake-3.31/Copyright.txt"]}


def remove_unused(name, root):
    keep = {}
    for path in KEEP.get(name, []):
        full = os.path.join(root, path)
        if os.path.isfile(full):
            keep[path] = open(full, "rb").read()
    for path in UNUSED.get(name, []):
        full = os.path.join(root, path)
        if os.path.isdir(full):
            shutil.rmtree(full)
        elif os.path.isfile(full):
            os.remove(full)
    for path, data in keep.items():
        full = os.path.join(root, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.write(data)
    for dirpath, dirs, files in os.walk(root, topdown=False):
        if dirpath != root and not os.listdir(dirpath):
            os.rmdir(dirpath)


def trim_llvm_mingw(root):
    """Leaves the i686 target: the other CPUs' libraries and tool names, the debugger and its Python go."""
    removed = 0
    for name in os.listdir(root):
        path = os.path.join(root, name)
        if os.path.isdir(path) and (name.startswith(OTHER_TARGETS) or name == "python"):
            shutil.rmtree(path)
            removed += 1
    bin_dir = os.path.join(root, "bin")
    for name in os.listdir(bin_dir):
        lower = name.lower()
        if lower.startswith(OTHER_TARGETS) or lower.startswith(("lldb", "liblldb")) or "i686-w64-mingw32uwp" in lower:
            os.remove(os.path.join(bin_dir, name))
            removed += 1
    clang_lib = os.path.join(root, "lib", "clang")
    for dirpath, _, files in os.walk(clang_lib):
        for name in files:
            if any(t in name for t in ("aarch64", "armv7", "x86_64", "arm64ec")):
                os.remove(os.path.join(dirpath, name))
    for name in os.listdir(os.path.join(root, "lib")):
        if name.startswith(("liblldb", "python")):
            path = os.path.join(root, "lib", name)
            shutil.rmtree(path) if os.path.isdir(path) else os.remove(path)
    return removed


def main():
    args = sys.argv[1:]
    downloads = None
    if "--downloads" in args:
        i = args.index("--downloads")
        downloads = args[i + 1]
        del args[i:i + 2]
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    root = os.path.abspath(args[0] if args else os.path.join(here, "toolchain"))
    downloads = os.path.abspath(downloads or os.path.join(root, "_downloads"))
    os.makedirs(downloads, exist_ok=True)

    linux = sys.platform.startswith("linux")
    packages, version = (LINUX_PACKAGES, LINUX_TOOLCHAIN_VERSION) if linux else (PACKAGES, TOOLCHAIN_VERSION)
    stamp = os.path.join(root, "TOOLCHAIN_VERSION")
    if os.path.isfile(stamp) and open(stamp).read().strip() == version:
        print("toolchain %s is ready in %s" % (version, root))
        return
    unpacked = set()
    for name, url, expected, folder, top in packages:
        archive = os.path.join(downloads, url.rsplit("/", 1)[1])
        download(url, archive, expected)
        dest = os.path.join(root, folder)
        if not archive.endswith((".zip", ".tar.gz", ".tar.xz", ".deb")):
            shutil.copyfile(archive, os.path.join(dest, os.path.basename(archive)))
            continue
        if os.path.isdir(dest) and folder not in unpacked:
            shutil.rmtree(dest)
        unpacked.add(folder)
        print("unpacking", name, flush=True)
        extract(archive, dest, top)
        if name == "llvm-mingw":
            trim_llvm_mingw(dest)
        remove_unused(name, dest)
    with open(stamp, "w") as f:
        f.write(version + "\n")
    print("toolchain %s is ready in %s" % (version, root))


if __name__ == "__main__":
    main()
