"""Compares two save-state dumps (DOJO_DUMP_STATE) variable by variable, even across builds.

    statediff.py A.bin A_stderr.txt A.pdb B.bin B_stderr.txt B.pdb

The stderr logs give the dumps' region addresses ("region i base size", printed with the dump); the PDBs
name the variables in the simulation's sections, compared by name, so that a build which adds variables
can still be checked against one without them. PSX RAM and the I/O page are compared byte for byte; the
VRAM copy and the game's stack are ignored. """

import re
import subprocess
import sys

PDBUTIL = r'C:\Program Files\LLVM\bin\llvm-pdbutil.exe'
NAMED_REGIONS = (0, 1)        # the simulation's data and bss sections
RAW_REGIONS = {2: 'psx ram', 3: 'i/o page'}


def regions(log):
    out = {}
    for line in open(log, errors='replace'):
        m = re.match(r'region (\d+) ([0-9a-f]+) (\d+)', line)
        if m:
            out[int(m.group(1))] = (int(m.group(2), 16), int(m.group(3)))
    return out


def symbols(pdb, everything=False):
    """address -> name of the variables (or, with everything, also the functions and other publics)."""
    args = [PDBUTIL, 'dump', '-symbols', '-globals', '-section-headers'] + (['-publics'] if everything else [])
    text = subprocess.run(args + [pdb], capture_output=True, text=True).stdout
    sections = {}
    index = 0
    for m in re.finditer(r'SECTION HEADER #(\d+).*?\n\s*\S+ name.*?\n.*?\n\s*([0-9A-F]+) virtual address', text,
                         re.S):
        sections[int(m.group(1))] = 0x400000 + int(m.group(2), 16)
    found = {}
    for m in re.finditer(r'S_[LG]DATA32 \[size = \d+\] `([^`]*)`\s*\n\s*type = [^,]*, addr = (\d+):(\d+)', text):
        name, sec, off = m.group(1), int(m.group(2)), int(m.group(3))
        if sec in sections:
            found[sections[sec] + off] = name
    if everything:
        for m in re.finditer(r'S_[LG]PROC32 \[size = \d+\] `([^`]*)`\s*\n\s*parent = [^\n]*addr = (\d+):(\d+)', text):
            sec, off = int(m.group(2)), int(m.group(3))
            if sec in sections:
                found.setdefault(sections[sec] + off, m.group(1))
        for m in re.finditer(r'S_PUB32 \[size = \d+\] `([^`]*)`\s*\n\s*flags = [^,]*, addr = (\d+):(\d+)', text):
            sec, off = int(m.group(2)), int(m.group(3))
            if sec in sections:
                found.setdefault(sections[sec] + off, m.group(1).lstrip('_'))
    return found


class Pointers:
    """Names an address in the image as symbol+offset, so pointers can be compared across builds."""

    def __init__(self, syms):
        import bisect
        self.bisect = bisect
        self.addrs = sorted(syms)
        self.syms = syms

    def name(self, value):
        if not 0x400000 <= value < 0x2000000 or not self.addrs:
            return None
        i = self.bisect.bisect_right(self.addrs, value) - 1
        if i < 0:
            return None
        a = self.addrs[i]
        return (self.syms[a], value - a)


def same(x, y, px, py):
    """The offsets at which two variables differ, pointers into the image compared by what they name."""
    import struct
    diffs = []
    n = min(len(x), len(y))
    i = 0
    while i < n:
        if x[i] == y[i]:
            i += 1
            continue
        w = i & ~3
        if w + 4 <= n:
            vx = struct.unpack_from('<I', x, w)[0]
            vy = struct.unpack_from('<I', y, w)[0]
            nx, ny = px.name(vx), py.name(vy)
            if nx is None and (vx >> 24) == (vy >> 24):  # a GPU packet's tag: length and 24-bit link
                nx, ny = px.name(vx & 0xFFFFFF), py.name(vy & 0xFFFFFF)
            if nx is not None and nx == ny:
                i = w + 4
                continue
        diffs.append(i)
        i += 1
    return diffs


def named(state, regs, syms):
    """name -> bytes for every variable in the named regions."""
    out = {}
    offset = 0
    for i in sorted(regs):
        base, size = regs[i]
        if i in NAMED_REGIONS:
            addrs = sorted(a for a in syms if base <= a < base + size)
            for k, a in enumerate(addrs):
                end = addrs[k + 1] if k + 1 < len(addrs) else base + size
                name = syms[a]
                while name in out:
                    name += "'"
                out[name] = state[offset + a - base:offset + end - base]
        offset += size
    return out


def raw(state, regs, index):
    offset = sum(regs[i][1] for i in sorted(regs) if i < index)
    return state[offset:offset + regs[index][1]]


def main():
    a_bin, a_log, a_pdb, b_bin, b_log, b_pdb = sys.argv[1:7]
    ra, rb = regions(a_log), regions(b_log)
    sa, sb = open(a_bin, 'rb').read(), open(b_bin, 'rb').read()
    va, vb = named(sa, ra, symbols(a_pdb)), named(sb, rb, symbols(b_pdb))
    pa, pb = Pointers(symbols(a_pdb, True)), Pointers(symbols(b_pdb, True))
    differ = 0
    for name in sorted(set(va) & set(vb)):
        x, y = va[name], vb[name]
        n = min(len(x), len(y))
        diffs = same(x, y, pa, pb)
        if diffs or len(x) != len(y):
            differ += 1
            first = diffs[0] if diffs else n
            print('%-40s size %d/%d, %d bytes differ, first at +%d' % (name, len(x), len(y), len(diffs), first))
    only_a = sorted(set(va) - set(vb))
    only_b = sorted(set(vb) - set(va))
    if only_a:
        print('only in A:', ' '.join(only_a))
    if only_b:
        print('only in B:', ' '.join(only_b))
    for index, label in RAW_REGIONS.items():
        x, y = raw(sa, ra, index), raw(sb, rb, index)
        diffs = [i for i in range(min(len(x), len(y))) if x[i] != y[i]]
        ranges = []
        for i in diffs:
            if ranges and i - ranges[-1][1] <= 16:
                ranges[-1][1] = i
            else:
                ranges.append([i, i])
        print('%s: %d bytes differ in %d ranges %s' % (
            label, len(diffs), len(ranges),
            ' '.join('%x-%x' % (ra[index][0] + s, ra[index][0] + e) for s, e in ranges[:12])))
    print('%d variables differ' % differ)


main()
