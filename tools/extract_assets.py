#!/usr/bin/env python3
"""Extract what the PC port's build needs from the player's own disc image.

Reads the ISO9660 file system straight out of a MODE2/2352 .bin, pulls the executable out of it and writes
each array of config/bushidoblade/assets.tsv that the build includes, checked against the table's SHA-256.

    python port/tools/extract_assets.py "game_files/Bushido Blade (USA).bin" port/build/generated
"""

import hashlib
import os
import struct
import sys

RAW_SECTOR = 2352
USER_OFFSET = 24  # sync (12) + header (4) + subheader (8) of a mode 2 form 1 sector
USER_SIZE = 2048

EXECUTABLES = {"SCUS": "SCUS_941.80", "SCES": "SCES_008.99", "SLPM": "SLPM_860.20"}


class Disc:
    def __init__(self, path):
        self.file = open(path, "rb")

    def sector(self, lba):
        self.file.seek(lba * RAW_SECTOR + USER_OFFSET)
        return self.file.read(USER_SIZE)

    def read(self, lba, size):
        out = bytearray()
        while len(out) < size:
            out += self.sector(lba)
            lba += 1
        return bytes(out[:size])

    def directory(self, lba, size):
        data = self.read(lba, size)
        entries = {}
        pos = 0
        while pos < len(data):
            length = data[pos]
            if length == 0:
                # Records do not cross sectors; skip to the next one.
                pos = (pos // USER_SIZE + 1) * USER_SIZE
                continue
            extent, = struct.unpack_from("<I", data, pos + 2)
            datalen, = struct.unpack_from("<I", data, pos + 10)
            flags = data[pos + 25]
            namelen = data[pos + 32]
            name = data[pos + 33:pos + 33 + namelen].decode("ascii", "replace")
            entries[name.split(";")[0]] = (extent, datalen, bool(flags & 2))
            pos += length
        return entries

    def root(self):
        pvd = self.sector(16)
        if pvd[1:6] != b"CD001":
            raise SystemExit("not an ISO9660 MODE2/2352 image")
        extent, = struct.unpack_from("<I", pvd, 156 + 2)
        datalen, = struct.unpack_from("<I", pvd, 156 + 10)
        return self.directory(extent, datalen)


def load_table(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        header = None
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            fields = line.rstrip("\n").split("\t")
            if header is None:
                header = fields
                continue
            rows.append(dict(zip(header, fields)))
    return rows


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    disc = Disc(sys.argv[1])
    out_dir = sys.argv[2]
    repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    root = disc.root()

    program = next((p for p, name in EXECUTABLES.items() if name in root), None)
    if program is None:
        raise SystemExit("no Bushido Blade executable on this disc")
    extent, size, _ = root[EXECUTABLES[program]]
    exe = disc.read(extent, size)
    if exe[:8] != b"PS-X EXE":
        raise SystemExit("the executable has no PS-X EXE header")
    text_addr, = struct.unpack_from("<I", exe, 0x18)
    print(f"{EXECUTABLES[program]}: {size} bytes, loaded at 0x{text_addr:08X}")

    for row in load_table(os.path.join(repo, "config", "bushidoblade", "assets.tsv")):
        if row["program"] != program:
            continue
        address = int(row["address"], 16)
        length = int(row["size"])
        offset = 0x800 + address - text_addr
        blob = exe[offset:offset + length]
        digest = hashlib.sha256(blob).hexdigest()
        if digest != row["sha256"]:
            raise SystemExit(f"{row['symbol']}: SHA-256 {digest} does not match the table")
        words = struct.unpack(f"<{length // 4}I", blob)
        target = os.path.join(out_dir, row["include"])
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, "w", encoding="ascii") as f:
            for i in range(0, len(words), 8):
                f.write(", ".join(f"0x{w:08X}" for w in words[i:i + 8]) + ",\n")
        print(f"{row['include']}: {row['symbol']}, {length} bytes, SHA-256 verified")


if __name__ == "__main__":
    main()
