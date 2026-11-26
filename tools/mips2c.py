#!/usr/bin/env python3
"""Translate the game's hand-written MIPS assembly units into C for the PC port.

The renderer (src/render.s) and three small drawers are hand-written R3000 assembly with their own register
conventions: the scene walk keeps its state in s0-s7 across calls, drawers return through labels in the
middle of other routines, and tables hold local code labels. This translator keeps all of that exact by
running every unit in one C function over a shared register file:

- each instruction becomes C on the register array R[], delay slots run before their branch takes effect;
- every instruction has a code address, CODE_BASE + 4 * its index, so a label's address plus a byte offset
  (a jump into unrolled code) means what it means on the console; branches and jumps to labels are gotos;
  calls set ra to a return id and jump; `jr` jumps through a switch over code addresses and return ids;
- a computed jump to an address that is not one of those labels is a C function (a table may hold C
  functions): it is called with a0-a3, its result goes to v0, and control returns through ra;
- each exported routine is a C function taking up to four words and returning v0, so C calls it as before;
- the console's scratchpad address maps to the port's scratchpad array; GTE instructions run on the
  port's software GTE.

    python port/tools/mips2c.py out.c src/render.s src/pov_flat.s ... [-D VERSION_JP] [-I include]
"""

import argparse
import os
import re
import sys

REGS = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]
REG_INDEX = {name: i for i, name in enumerate(REGS)}
REG_INDEX["s8"] = 30
for i in range(32):
    REG_INDEX[f"${i}"] = i
    REG_INDEX[f"r{i}"] = i

GTE_DATA = ["VXY0", "VZ0", "VXY1", "VZ1", "VXY2", "VZ2", "RGB", "OTZ", "IR0", "IR1", "IR2", "IR3", "SXY0",
            "SXY1", "SXY2", "SXYP", "SZ0", "SZ1", "SZ2", "SZ3", "RGB0", "RGB1", "RGB2", "RES1", "MAC0", "MAC1",
            "MAC2", "MAC3", "IRGB", "ORGB", "LZCS", "LZCR"]
GTE_CTRL = ["R11R12", "R13R21", "R22R23", "R31R32", "R33", "TRX", "TRY", "TRZ", "L11L12", "L13L21", "L22L23",
            "L31L32", "L33", "RBK", "GBK", "BBK", "LR1LR2", "LR3LG1", "LG2LG3", "LB1LB2", "LB3", "RFC", "GFC",
            "BFC", "OFX", "OFY", "H", "DQA", "DQB", "ZSF3", "ZSF4", "FLAG"]
GTE_DATA_INDEX = {f"C2_{n}": i for i, n in enumerate(GTE_DATA)}
GTE_CTRL_INDEX = {f"C2_{n}": i for i, n in enumerate(GTE_CTRL)}

# GTE command words; a trailing parameter list ORs in sf (and MVMVA's matrix, vector, addend, lm).
GTE_COMMANDS = {
    "RTPS": 0x0180001, "RTPT": 0x0280030, "NCLIP": 0x1400006, "OP": 0x170000C, "DPCS": 0x0780010,
    "INTPL": 0x0980011, "MVMVA": 0x0400012, "NCDS": 0x0E80413, "CDP": 0x1280414, "NCDT": 0x0F80416,
    "NCCS": 0x108041B, "CC": 0x138041C, "NCS": 0x0C8041E, "NCT": 0x0D80420, "SQR": 0x0A00428,
    "DCPL": 0x0680029, "DPCT": 0x0F8002A, "AVSZ3": 0x158002D, "AVSZ4": 0x168002E, "GPF": 0x190003D,
    "GPL": 0x1A0003E, "NCCT": 0x118043F,
}
# Commands the kit's macros issue with sf=1 when no parameter is given.
GTE_DEFAULT_SF = {"RTPS", "RTPT", "DPCS", "INTPL", "NCDS", "CDP", "NCDT", "NCCS", "CC", "NCS", "NCT", "DCPL",
                  "DPCT", "NCCT"}

SCRATCH = "__SCRATCH__"


class AsmError(Exception):
    pass


class Value:
    """An assembly-time value: a constant, or a symbol's address plus a constant."""

    def __init__(self, const=0, sym=None, hi=False):
        self.const = const
        self.sym = sym
        self.hi = hi  # the symbol's address >> 16, which only lui may take

    def require_const(self, what):
        if self.sym is not None:
            raise AsmError(f"{what}: needs a constant, got {self.sym}")
        return self.const


class Expr:
    """A recursive-descent evaluator for the assembler's expressions."""

    TOKEN = re.compile(r"\s*(\$[0-9A-Fa-f]+|0[xX][0-9A-Fa-f]+|\d+|[A-Za-z_.][A-Za-z0-9_.]*|<<|>>|[-+*/()&|^~])")

    def __init__(self, text, lookup):
        self.tokens = []
        pos = 0
        text = text.strip()
        while pos < len(text):
            m = self.TOKEN.match(text, pos)
            if not m:
                raise AsmError(f"bad expression: {text!r}")
            self.tokens.append(m.group(1))
            pos = m.end()
        self.pos = 0
        self.lookup = lookup

    def parse(self):
        v = self.binary(0)
        if self.pos != len(self.tokens):
            raise AsmError(f"trailing tokens in expression: {self.tokens}")
        return v

    LEVELS = [("|",), ("^",), ("&",), ("<<", ">>"), ("+", "-"), ("*", "/")]

    def binary(self, level):
        if level == len(self.LEVELS):
            return self.unary()
        left = self.binary(level + 1)
        while self.pos < len(self.tokens) and self.tokens[self.pos] in self.LEVELS[level]:
            op = self.tokens[self.pos]
            self.pos += 1
            right = self.binary(level + 1)
            left = self.apply(op, left, right)
        return left

    def apply(self, op, a, b):
        if op == "+":
            if a.sym and b.sym:
                raise AsmError("adding two symbols")
            return Value(a.const + b.const, a.sym or b.sym)
        if op == "-":
            if b.sym:
                raise AsmError("subtracting a symbol")
            return Value(a.const - b.const, a.sym)
        if op == ">>" and a.sym is not None:
            if b.require_const(">>") != 16 or a.const != 0:
                raise AsmError("only symbol>>16 is supported")
            return Value(0, a.sym, hi=True)
        x = a.require_const(op)
        y = b.require_const(op)
        return Value({"*": lambda: x * y, "/": lambda: int(x / y), "<<": lambda: x << y, ">>": lambda: x >> y,
                      "&": lambda: x & y, "|": lambda: x | y, "^": lambda: x ^ y}[op]())

    def unary(self):
        tok = self.tokens[self.pos]
        self.pos += 1
        if tok == "-":
            return Value(-self.unary().require_const("-"))
        if tok == "~":
            return Value(~self.unary().require_const("~"))
        if tok == "(":
            v = self.binary(0)
            if self.tokens[self.pos] != ")":
                raise AsmError("missing )")
            self.pos += 1
            return v
        if tok.startswith("$"):
            return Value(int(tok[1:], 16))
        if tok[0].isdigit():
            return Value(int(tok, 0))
        if tok in ("HI", "LO") and self.pos < len(self.tokens) and self.tokens[self.pos] == "(":
            # A relocation pair: lui takes HI(sym) and a later offset LO(sym). The translation gives lui the
            # whole address and the offset nothing, so the pair still sums to the symbol's address.
            self.pos += 1
            inner = self.binary(0)
            self.pos += 1
            if tok == "LO":
                return Value(inner.const & 0xFFFF if inner.sym is None else 0)
            if inner.sym is None:
                return Value(((inner.const + 0x8000) >> 16) & 0xFFFF)
            return Value(inner.const, inner.sym, hi=True)
        return self.lookup(tok)


class Line:
    def __init__(self, file, number, label, op, args, text):
        self.file = file
        self.number = number
        self.label = label
        self.op = op
        self.args = args
        self.text = text

    def where(self):
        return f"{os.path.basename(self.file)}:{self.number}"


def split_args(text):
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
            continue
        depth += ch == "("
        depth -= ch == ")"
        cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


class Assembler:
    def __init__(self, include_dirs, defines):
        self.include_dirs = include_dirs
        self.defines = set(defines)
        self.equ = {}        # name -> expression text, evaluated on demand
        self.consts = {}     # name -> evaluated constant
        self.lines = []      # code and data lines in order, with their section
        self.exports = set()
        self.externs = set()
        self.file_locals = {}

    # -- reading ---------------------------------------------------------------------------------------

    def find_include(self, name):
        for d in self.include_dirs:
            path = os.path.join(d, name)
            if os.path.exists(path):
                return path
        return None

    def read_file(self, path, unit):
        stack = []
        active = True
        with open(path, encoding="utf-8") as f:
            raw = f.read().split("\n")
        rs = getattr(self, "rs", 0)
        section = getattr(self, "section", ".text")
        for number, text in enumerate(raw, 1):
            stripped = text.split(";", 1)[0].rstrip()
            if stripped.startswith("#"):
                parts = stripped.split()
                if parts[0] == "#ifdef":
                    stack.append(active)
                    active = active and parts[1] in self.defines
                elif parts[0] == "#ifndef":
                    stack.append(active)
                    active = active and parts[1] not in self.defines
                elif parts[0] == "#else":
                    active = stack[-1] and not active
                elif parts[0] == "#endif":
                    active = stack.pop()
                continue  # #include <gtereg.h>: the GTE register names are built in
            if not active or not stripped.strip():
                continue
            label = None
            body = stripped
            if not stripped[0].isspace():
                m = re.match(r"([A-Za-z_.][A-Za-z0-9_.]*):?(.*)", stripped)
                label, body = m.group(1), m.group(2)
            parts = body.strip().split(None, 1)
            op = parts[0] if parts else None
            args = split_args(parts[1]) if len(parts) > 1 else []
            if op is not None and op.lower() == "include":
                name = args[0].strip('"')
                if name in ("INLINE_A.H", "gte_word.inc"):
                    continue  # GTE commands are built in
                inc = self.find_include(name)
                if inc is None:
                    raise AsmError(f"{path}:{number}: include {name} not found")
                self.rs, self.section = rs, section
                self.read_file(inc, unit)
                rs, section = self.rs, self.section
                continue
            if op == "equ":
                self.equ[label] = args[0] if len(args) == 1 else ",".join(args)
                continue
            if op == "rsreset":
                rs = 0
                continue
            if op == "rsset":
                rs = self.eval(args[0]).require_const("rsset")
                continue
            if op in ("rb", "rh", "rw"):
                size = {"rb": 1, "rh": 2, "rw": 4}[op] * self.eval(args[0]).require_const(op)
                if label:
                    self.consts[label] = rs
                rs += size
                continue
            if op in ("opt", "macro", "endm"):
                continue
            if op == "section":
                section = args[0]
                continue
            if op == "xdef":
                self.exports.add(args[0])
                continue
            if op == "xref":
                self.externs.add(args[0])
                continue
            if label and label.startswith("."):
                label = f"{unit}{label}"
            self.lines.append(Line(path, number, label, op, args, text.strip()))
            self.lines[-1].section = section
            self.lines[-1].unit = unit
        self.rs, self.section = rs, section

    # -- symbols ---------------------------------------------------------------------------------------

    def eval(self, text, unit=None):
        def lookup(name):
            if name.startswith(".") and unit is not None:
                return Value(0, f"{unit}{name}")
            if name in self.consts:
                return Value(self.consts[name])
            if name == "SCRATCHPAD":
                return Value(0, SCRATCH)
            if name in self.equ:
                v = Expr(self.equ[name], lookup).parse()
                if v.sym is None:
                    self.consts[name] = v.const
                return v
            return Value(0, name)
        return Expr(text, lookup).parse()


def c_ident(name):
    return re.sub(r"[^A-Za-z0-9_]", "_", name)


class Translator:
    DATA_DIRECTIVES = ("dw", "dh", "db", "dsw", "dsh", "dsb")

    def __init__(self, asm):
        self.asm = asm
        # Data written among the code (LIBPRESS's VLC decoder keeps its state there) goes with the data, the
        # labels just before it with it.
        text, data, pending = [], [], []
        for l in asm.lines:
            if l.section != ".text":
                data.append(l)
            elif l.op is None:
                pending.append(l)
            elif l.op in self.DATA_DIRECTIVES:
                data.extend(pending)
                data.append(l)
                pending = []
            else:
                text.extend(pending)
                text.append(l)
                pending = []
        text.extend(pending)
        self.code = self.attach_labels(text)
        self.data = data
        self.code_labels = {label for l in self.code for label in l.labels}
        self.label_index = {label: i for i, l in enumerate(self.code) for label in l.labels}
        self.data_labels = {l.label for l in self.data if l.label}
        self.out = []
        self.ret_count = 0
        self.skip_count = 0
        self.c_functions = set()     # extern symbols called with jal
        self.c_data = set()          # extern symbols used as data
        self.data_offsets = {}
        self.sites = []

    @staticmethod
    def attach_labels(lines):
        """Gives each instruction the labels written before it (a label may stand on a line of its own)."""
        out, pending = [], []
        for line in lines:
            if line.label:
                pending.append(line.label)
            if line.op is None:
                continue
            line.labels = pending
            pending = []
            out.append(line)
        if pending:
            end = Line(lines[-1].file, lines[-1].number, None, "nop", [], "; end of code")
            end.labels, end.unit, end.section = pending, lines[-1].unit, ".text"
            out.append(end)
        return out

    def reg(self, text, line):
        t = text.strip()
        if t not in REG_INDEX:
            raise AsmError(f"{line.where()}: not a register: {t!r}")
        return REG_INDEX[t]

    def r(self, i):
        return "0u" if i == 0 else f"R[{i}]"

    def set_reg(self, i, expr):
        return "" if i == 0 else f"R[{i}] = (u32)({expr});"

    def value_c(self, v, line):
        """C for an assembly value: a constant, or a symbol's address plus a constant."""
        if v.hi:
            # Only lui takes a symbol's high half; it receives the whole address (see Expr.unary).
            return f"({self.sym_c(v.sym, line)} + 0x{v.const & 0xFFFFFFFF:X}u)"
        const = v.const & 0xFFFFFFFF
        if v.sym is None:
            return f"0x{const:X}u"
        return f"({self.sym_c(v.sym, line)} + 0x{const:X}u)"

    def sym_c(self, sym, line):
        if sym == SCRATCH:
            return "SCRATCH_BASE"
        if sym in self.code_labels:
            return f"(CODE_BASE + {4 * self.label_index[sym]}u)"
        if sym in self.data_labels:
            return f"(ADDR(sData) + {self.data_offsets[sym]}u)"
        self.c_data.add(sym)
        return f"ADDR({c_ident(sym)})"

    def mem_address(self, text, line):
        """The C address of a memory operand: off(base), off alone, or a symbol."""
        m = re.match(r"^(.*)\(\s*([$A-Za-z0-9]+)\s*\)$", text.strip())
        if m:
            off_text, base = m.group(1).strip(), self.reg(m.group(2), line)
            off = self.asm.eval(off_text, line.unit) if off_text else Value(0)
            base_c = self.r(base)
            if off.sym is None:
                return f"({base_c} + 0x{off.const & 0xFFFFFFFF:X}u)"
            return f"({base_c} + {self.value_c(off, line)})"
        return self.value_c(self.asm.eval(text, line.unit), line)

    def new_ret(self):
        self.ret_count += 1
        return f"RET_{self.ret_count}"

    # -- instructions ----------------------------------------------------------------------------------

    ALU3 = {"addu": "+", "add": "+", "subu": "-", "sub": "-", "and": "&", "or": "|", "xor": "^"}
    ALUI = {"addiu": "+", "addi": "+", "andi": "&", "ori": "|", "xori": "^"}
    LOADS = {"lw": ("u32", "u32"), "lh": ("s16", "s32"), "lhu": ("u16", "u32"), "lb": ("s8", "s32"),
             "lbu": ("u8", "u32")}
    STORES = {"sw": "u32", "sh": "u16", "sb": "u8"}
    BRANCH1 = {"blez": "<= 0", "bgtz": "> 0", "bltz": "< 0", "bgez": ">= 0", "beqz": "== 0", "bnez": "!= 0"}

    def simple(self, line):
        """C for a non-control instruction."""
        op, a = line.op, line.args
        if getattr(line, "wide_reject", False):
            d, rs, rt = (self.reg(x, line) for x in (a if len(a) == 3 else [a[0], a[0], a[1]]))
            return self.set_reg(d, f"WideRejectX({self.r(rs)}) & WideRejectX({self.r(rt)})")
        if op == "nop":
            return ""
        if op in self.ALU3:
            if len(a) == 2:
                a = [a[0], a[0], a[1]]
            d, s = self.reg(a[0], line), self.reg(a[1], line)
            if a[2].strip() in REG_INDEX:
                t = self.r(self.reg(a[2], line))
            else:
                t = self.value_c(self.asm.eval(a[2], line.unit), line)
            return self.set_reg(d, f"{self.r(s)} {self.ALU3[op]} {t}")
        if op in self.ALUI:
            if len(a) == 2:
                a = [a[0], a[0], a[1]]
            d, s = self.reg(a[0], line), self.reg(a[1], line)
            v = self.asm.eval(a[2], line.unit)
            if v.sym is not None:
                return self.set_reg(d, f"{self.r(s)} {self.ALUI[op]} {self.value_c(v, line)}")
            imm = v.const
            if op in ("andi", "ori", "xori"):
                imm &= 0xFFFF
            return self.set_reg(d, f"{self.r(s)} {self.ALUI[op]} 0x{imm & 0xFFFFFFFF:X}u")
        if op in ("slt", "sltu", "slti", "sltiu"):
            if len(a) == 2:
                a = [a[0], a[0], a[1]]
            d, s = self.reg(a[0], line), self.reg(a[1], line)
            if op in ("slt", "sltu") and a[2].strip() in REG_INDEX:
                t = self.r(self.reg(a[2], line))
            else:
                t = f"0x{self.asm.eval(a[2], line.unit).require_const(op) & 0xFFFFFFFF:X}u"
            if op in ("slt", "slti"):
                return self.set_reg(d, f"(s32){self.r(s)} < (s32){t}")
            return self.set_reg(d, f"{self.r(s)} < {t}")
        if op == "nor":
            d, s, t = (self.reg(x, line) for x in a)
            return self.set_reg(d, f"~({self.r(s)} | {self.r(t)})")
        if op in ("sll", "srl", "sra"):
            if len(a) == 2:
                a = [a[0], a[0], a[1]]
            d, t = self.reg(a[0], line), self.reg(a[1], line)
            sa = self.asm.eval(a[2], line.unit).require_const(op) & 31
            if op == "sll":
                return self.set_reg(d, f"{self.r(t)} << {sa}")
            if op == "srl":
                return self.set_reg(d, f"{self.r(t)} >> {sa}")
            return self.set_reg(d, f"(s32){self.r(t)} >> {sa}")
        if op in ("sllv", "srlv", "srav"):
            if len(a) == 2:
                a = [a[0], a[0], a[1]]
            d, t, s = (self.reg(x, line) for x in a)
            sh = f"({self.r(s)} & 31)"
            if op == "sllv":
                return self.set_reg(d, f"{self.r(t)} << {sh}")
            if op == "srlv":
                return self.set_reg(d, f"{self.r(t)} >> {sh}")
            return self.set_reg(d, f"(s32){self.r(t)} >> {sh}")
        if op in ("mult", "multu", "div", "divu"):
            s, t = self.reg(a[0], line), self.reg(a[1], line)
            if op == "mult":
                return f"{{ s64 p = (s64)(s32){self.r(s)} * (s32){self.r(t)}; LO = (u32)p; HI = (u32)(p >> 32); }}"
            if op == "multu":
                return f"{{ u64 p = (u64){self.r(s)} * {self.r(t)}; LO = (u32)p; HI = (u32)(p >> 32); }}"
            if op == "div":
                return f"DivSigned({self.r(s)}, {self.r(t)});"
            return f"DivUnsigned({self.r(s)}, {self.r(t)});"
        if op in ("mflo", "mfhi"):
            return self.set_reg(self.reg(a[0], line), "LO" if op == "mflo" else "HI")
        if op in ("mtlo", "mthi"):
            return f"{'LO' if op == 'mtlo' else 'HI'} = {self.r(self.reg(a[0], line))};"
        if op in ("move",):
            return self.set_reg(self.reg(a[0], line), self.r(self.reg(a[1], line)))
        if op == "lui":
            v = self.asm.eval(a[1], line.unit)
            if v.hi:
                return self.set_reg(self.reg(a[0], line), self.value_c(v, line))
            return self.set_reg(self.reg(a[0], line), f"0x{(v.require_const('lui') << 16) & 0xFFFFFFFF:X}u")
        if op in ("li", "la"):
            return self.set_reg(self.reg(a[0], line), self.value_c(self.asm.eval(a[1], line.unit), line))
        if op in self.LOADS:
            ctype, wide = self.LOADS[op]
            return self.set_reg(self.reg(a[0], line), f"({wide})*({ctype} *)PTR({self.mem_address(a[1], line)})")
        if op in self.STORES:
            ctype = self.STORES[op]
            return f"*({ctype} *)PTR({self.mem_address(a[1], line)}) = ({ctype}){self.r(self.reg(a[0], line))};"
        if op in ("mfc0", "mtc0"):
            # The system coprocessor: the only use (LIBPRESS's VLC decoder) swaps the caches in the status
            # register when it is done, which means nothing to the port. Reads give 0.
            if a[1].strip() != "status":
                raise SystemExit(f"{line.unit}: {op} of {a[1].strip()} is not translated")
            return self.set_reg(self.reg(a[0], line), "0u") if op == "mfc0" else ";"
        if op == "mtc2":
            return f"GteWriteData({GTE_DATA_INDEX[a[1].strip()]}, {self.r(self.reg(a[0], line))});"
        if op == "mfc2":
            return self.set_reg(self.reg(a[0], line), f"GteReadData({GTE_DATA_INDEX[a[1].strip()]})")
        if op == "ctc2":
            return f"GteWriteCtrl({GTE_CTRL_INDEX[a[1].strip()]}, {self.r(self.reg(a[0], line))});"
        if op == "cfc2":
            return self.set_reg(self.reg(a[0], line), f"GteReadCtrl({GTE_CTRL_INDEX[a[1].strip()]})")
        if op == "lwc2":
            return f"GteWriteData({GTE_DATA_INDEX[a[0].strip()]}, *(u32 *)PTR({self.mem_address(a[1], line)}));"
        if op == "swc2":
            return f"*(u32 *)PTR({self.mem_address(a[1], line)}) = GteReadData({GTE_DATA_INDEX[a[0].strip()]});"
        gte = self.gte_command(op, a, line)
        if gte is not None:
            return f"GteExecute(0x{gte:07X});"
        raise AsmError(f"{line.where()}: unsupported instruction {line.text!r}")

    def gte_command(self, op, args, line):
        name = op.upper() if op.upper() in GTE_COMMANDS else op
        if name[0] in "wn" and name[1:] in GTE_COMMANDS:
            name = name[1:]
        if name not in GTE_COMMANDS:
            return None
        word = GTE_COMMANDS[name]
        params = [self.asm.eval(x, line.unit).require_const(op) for x in args]
        if name == "MVMVA":
            sf, mx, v, cv, lm = params
            return word | sf << 19 | mx << 17 | v << 15 | cv << 13 | lm << 10
        if params:
            return word | params[0] << 19
        if name in GTE_DEFAULT_SF:
            return word | 1 << 19
        return word

    # -- control flow ----------------------------------------------------------------------------------

    def target_label(self, text, line):
        name = text.strip()
        if name.startswith("."):
            name = f"{line.unit}{name}"
        return name

    def emit(self, text):
        self.out.append(text)

    def add_site(self, line):
        """Records a computed jump's source line, for the report of a jump to an address that is not code."""
        self.sites.append(f"{line.where()}: {line.text}")
        return len(self.sites) - 1

    def mark_reject_chains(self):
        """Widescreen: the drawers reject a face whose corners are all left of the screen by ANDing the corners'
        packed positions and testing bit 15 (`andi R,R,$8000`), bit 31 for all above it. The `and`s that
        build R are marked to test each corner's x against -g_pcScreenMargin instead of 0, so faces in the
        margin a wider screen shows are drawn; with no margin the bits come out as the console's. R must be
        read by nothing else between them: the Y test's bltz only."""
        lines = self.code
        for i, line in enumerate(lines):
            if line.op != "andi" or len(line.args) != 3:
                continue
            reg = line.args[0].strip()
            if line.args[1].strip() != reg or line.args[2].strip() != "$8000":
                continue
            chain = []
            j = i - 1
            while j >= 0 and i - j <= 24:
                prev = lines[j]
                if prev.op == "and" and prev.args and prev.args[0].strip() == reg:
                    chain.append(j)
                    if prev.args[1].strip() != reg:
                        break  # the chain's first and: both operands are corners
                j -= 1
            if not chain:
                continue
            for k in range(min(chain), i):
                other = lines[k]
                if k in chain or (other.op == "bltz" and other.args[0].strip() == reg):
                    continue
                if any(re.search(r"%s" % re.escape(reg), arg) for arg in other.args):
                    raise AsmError(f"{other.where()}: {reg} read inside a reject chain")
            for k in chain:
                lines[k].wide_reject = True

    def goto_label(self, label):
        return f"I_{self.label_index[label]}"

    def translate_code(self):
        """Every instruction gets the label I_<index>, so a jump may land on any of them: hand-written code
        computes targets as a label plus a byte offset (CopyWordsFast jumps into its unrolled copy)."""
        lines = self.code
        i = 0
        while i < len(lines):
            line = lines[i]
            self.emit(f"I_{i}: ;")
            op = line.op
            control = op in ("beq", "bne", "b", "j", "jal", "jr", "jalr") or op in self.BRANCH1
            if not control:
                if op in ("dw", "dh", "db"):
                    raise AsmError(f"{line.where()}: data in .text")
                self.emit(f"    {self.simple(line)} /* {line.text} */")
                i += 1
                continue
            slot = lines[i + 1] if i + 1 < len(lines) else None
            if slot is None:
                raise AsmError(f"{line.where()}: branch without a delay slot")
            slot_c = self.simple(slot)
            self.emit(f"    /* {line.text} ; {slot.text} */")
            a = line.args
            ret = None
            falls_through = False
            if op in ("beq", "bne") or op in self.BRANCH1 or op == "b":
                if op == "b":
                    cond = "1"
                    target = a[0]
                elif op in ("beq", "bne"):
                    rs, rt = self.reg(a[0], line), self.reg(a[1], line)
                    relation = "==" if op == "beq" else "!="
                    cond = f"{self.r(rs)} {relation} {self.r(rt)}"
                    target = a[2]
                else:
                    cond = f"(s32){self.r(self.reg(a[0], line))} {self.BRANCH1[op]}"
                    target = a[1]
                label = self.target_label(target, line)
                if label not in self.code_labels:
                    raise AsmError(f"{line.where()}: branch to unknown label {label}")
                self.emit(f"    {{ int taken = {cond}; {slot_c} if (taken) goto {self.goto_label(label)}; }}")
                falls_through = op != "b"
            elif op == "j":
                label = self.target_label(a[0], line)
                if label in self.code_labels:
                    self.emit(f"    {{ {slot_c} goto {self.goto_label(label)}; }}")
                else:
                    self.c_functions.add(label)
                    self.emit(f"    {{ {slot_c} R[2] = CALL_C({c_ident(label)}); pc = R[31]; goto dispatch; }}")
            elif op == "jal":
                label = self.target_label(a[0], line)
                ret = self.new_ret()
                if label in self.code_labels:
                    self.emit(f"    {{ {slot_c} R[31] = {ret}; goto {self.goto_label(label)}; }}")
                else:
                    self.c_functions.add(label)
                    self.emit(f"    {{ {slot_c} R[31] = {ret}; R[2] = CALL_C({c_ident(label)}); goto {ret}_L; }}")
            elif op == "jr":
                rs = self.reg(a[0], line)
                site = self.add_site(line)
                self.emit(f"    {{ u32 dest = {self.r(rs)}; {slot_c} pc = dest; sSite = {site}; goto dispatch; }}")
            elif op == "jalr":
                if len(a) == 2:
                    rd, rs = self.reg(a[0], line), self.reg(a[1], line)
                else:
                    rd, rs = 31, self.reg(a[0], line)
                ret = self.new_ret()
                site = self.add_site(line)
                self.emit(f"    {{ u32 dest = {self.r(rs)}; {slot_c} R[{rd}] = {ret}; pc = dest; sSite = {site}; "
                          f"goto dispatch; }}")
            # The delay slot as a jump target of its own: run it and go on with what follows it.
            self.skip_count += 1
            after = f"AFTER_{self.skip_count}"
            if falls_through:
                self.emit(f"    goto {after};")
            self.emit(f"I_{i + 1}: ;")
            self.emit(f"    {slot_c} /* {slot.text} */")
            self.emit(f"{after}: ;")
            if ret is not None:
                self.emit(f"{ret}_L: ;")
            i += 2
        self.emit(f"I_{len(lines)}: ;")
        self.code_count = len(lines)

    # -- data ------------------------------------------------------------------------------------------

    def translate_data(self):
        """Lays the units' data out as the assembler did, in one block; returns (decls, init statements)."""
        widths = {"dw": 4, "dh": 2, "db": 1, "dsw": 4, "dsh": 2, "dsb": 1}
        offset = 0
        items = []
        for line in self.data:
            if line.label:
                self.data_offsets[line.label] = offset
            if line.op is None:
                continue
            if line.op not in widths:
                raise AsmError(f"{line.where()}: unsupported data directive {line.op}")
            size = widths[line.op]
            if line.op.startswith("ds"):
                offset += size * self.asm.eval(line.args[0], line.unit).require_const(line.op)
                continue
            for arg in line.args:
                items.append((offset, size, arg, line))
                offset += size
        blob = bytearray((offset + 3) & ~3)
        inits = []
        ctype = {4: "u32", 2: "u16", 1: "u8"}
        for off, size, arg, line in items:
            v = self.asm.eval(arg, line.unit)
            if v.sym is None:
                blob[off:off + size] = (v.const & ((1 << (8 * size)) - 1)).to_bytes(size, "little")
            else:
                inits.append(f"    *({ctype[size]} *)&sData[{off}] = ({ctype[size]}){self.value_c(v, line)};")
        rows = []
        for i in range(0, len(blob), 16):
            rows.append("    " + ", ".join(f"0x{b:02X}" for b in blob[i:i + 16]) + ",")
        decls = [f"static __attribute__((aligned(16))) u8 sData[{max(len(blob), 4)}] = {{"] + rows + ["};"]
        return decls, inits

    # -- output ----------------------------------------------------------------------------------------

    def generate(self):
        data_decls, data_inits = self.translate_data()
        self.mark_reject_chains()
        self.translate_code()
        wrappers = sorted(self.asm.exports & self.code_labels)

        out = []
        out.append("/* Generated by port/tools/mips2c.py from the game's hand-written assembly. Do not edit. */")
        out.append("")
        out.append("#include <stdint.h>")
        out.append('#include "gte_soft.h"')
        out.append("")
        out.append("typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;")
        out.append("typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;")
        out.append("")
        out.append("extern unsigned char g_psxScratch[];")
        out.append("#define SCRATCH_BASE ((u32)(uintptr_t)g_psxScratch)")
        out.append("#define ADDR(sym) ((u32)(uintptr_t)(sym))")
        out.append("#define PTR(addr) ((void *)(uintptr_t)(u32)(addr))")
        out.append("#define CALL_C(fn) ((u32 (*)(u32, u32, u32, u32))(fn))(R[4], R[5], R[6], R[7])")
        out.append("/* Entry points take up to ten words; the fifth and later go on the emulated stack (Enter). */")
        out.append("#define ARG_MAX 10")
        out.append("#define ARGS u32 a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8, u32 a9")
        out.append("#define ENTER(label) Enter((label), (const u32[]){a0, a1, a2, a3, a4, a5, a6, a7, a8, a9})")
        out.append("")
        out.append("static u32 R[32];")
        out.append("static u32 HI, LO;")
        out.append("static u32 sStack[0x10000];")
        out.append("void SaveStateAddDeadStack(void *base, unsigned size, const unsigned *sp);")
        out.append("")
        out.append("/* A corner's packed position for the drawers' off-screen test, its x read as left of the screen only")
        out.append(" * past the widescreen margin (Translator.mark_reject_chains). */")
        out.append("extern int g_pcScreenMargin;")
        out.append("static u32 WideRejectX(u32 w)")
        out.append("{")
        out.append("    return (w & 0xFFFF0000u) | ((s32)(s16)w < -g_pcScreenMargin ? 0x8000u : 0u);")
        out.append("}")
        out.append("")
        out.append("static void DivSigned(u32 n, u32 d)")
        out.append("{")
        out.append("    s32 sn = (s32)n, sd = (s32)d;")
        out.append("    if (sd == 0) { LO = sn >= 0 ? 0xFFFFFFFFu : 1u; HI = n; return; }")
        out.append("    if (sn == INT32_MIN && sd == -1) { LO = n; HI = 0; return; }")
        out.append("    LO = (u32)(sn / sd); HI = (u32)(sn % sd);")
        out.append("}")
        out.append("")
        out.append("static void DivUnsigned(u32 n, u32 d)")
        out.append("{")
        out.append("    if (d == 0) { LO = 0xFFFFFFFFu; HI = n; return; }")
        out.append("    LO = n / d; HI = n % d;")
        out.append("}")
        out.append("")
        for sym in sorted(self.c_functions):
            out.append(f"extern u32 {c_ident(sym)}();")
        for sym in sorted(self.c_data - self.c_functions):
            out.append(f"extern char {c_ident(sym)}[];")
        out.append("")
        for w in wrappers:
            storage = "" if w in self.asm.exports else "static "
            out.append(f"{storage}u32 W_{c_ident(w)}(ARGS);")
        out.append("")
        out.extend(data_decls)
        out.append("")
        out.append("/* Code addresses: instruction i of the units is CODE_BASE + 4 * i, so label arithmetic works. */")
        out.append("#define CODE_BASE 0xC0000000u")
        out.append(f"#define CODE_COUNT {self.code_count + 1}u")
        out.append("#define RET_TO_C 0xFFFFu")
        for n in range(1, self.ret_count + 1):
            out.append(f"#define RET_{n} {0x10000 + n}u")
        out.append("")
        out.append("static const struct { u32 (*fn)(ARGS); u32 id; } sCodeAddresses[] = {")
        for w in wrappers:
            out.append(f"    {{ W_{c_ident(w)}, {self.label_index[w]} }},")
        out.append("};")
        out.append("")
        out.append("static u32 LookupCode(u32 addr)")
        out.append("{")
        out.append("    unsigned i;")
        out.append("    for (i = 0; i < sizeof(sCodeAddresses) / sizeof(sCodeAddresses[0]); i++) {")
        out.append("        if (ADDR(sCodeAddresses[i].fn) == addr) {")
        out.append("            return CODE_BASE + 4 * sCodeAddresses[i].id;")
        out.append("        }")
        out.append("    }")
        out.append("    return 0;")
        out.append("}")
        out.append("")
        out.append("/* The computed jumps' source lines, for AsmBadJump. */")
        out.append("static const char *const sSiteText[] = {")
        for text in self.sites or ["(none)"]:
            escaped = text.replace("\\", "\\\\").replace('"', '\\"')
            out.append(f'    "{escaped}",')
        out.append("};")
        out.append("static u32 sSite;")
        # the executable's first byte: the PE linker's __ImageBase, the ELF linker's __executable_start
        out.append("#ifdef _WIN32")
        out.append("extern char __ImageBase;")
        out.append("#define IMAGE_START (&__ImageBase)")
        out.append("#else")
        out.append("extern char __executable_start;")
        out.append("#define IMAGE_START (&__executable_start)")
        out.append("#endif")
        out.append("void AsmBadJump(u32 pc, const char *site, const u32 *regs);")
        out.append("")
        out.append("static void Run(u32 pc)")
        out.append("{")
        out.append("dispatch:")
        out.append("    if (pc - CODE_BASE < CODE_COUNT * 4) {")
        out.append("        switch ((pc - CODE_BASE) >> 2) {")
        for n in range(self.code_count + 1):
            out.append(f"        case {n}: goto I_{n};")
        out.append("        }")
        out.append("    }")
        out.append("    switch (pc) {")
        out.append("    case RET_TO_C: return;")
        for n in range(1, self.ret_count + 1):
            out.append(f"    case RET_{n}: goto RET_{n}_L;")
        out.append("    default: break;")
        out.append("    }")
        out.append("    {")
        out.append("        u32 id = LookupCode(pc);")
        out.append("        if (id != 0) { pc = id; goto dispatch; }")
        out.append("        /* A C function reached through a table or a tail call: call it, return through ra. */")
        out.append("        if (pc < ADDR(IMAGE_START) || pc >= ADDR(IMAGE_START) + 0x1000000u) {")
        out.append("            AsmBadJump(pc, sSiteText[sSite], R);")
        out.append("        }")
        out.append("        R[2] = ((u32 (*)(u32, u32, u32, u32))PTR(pc))(R[4], R[5], R[6], R[7]);")
        out.append("        pc = R[31];")
        out.append("        goto dispatch;")
        out.append("    }")
        out.extend(self.out)
        out.append("    return; /* fell off the end of the code */")
        out.append("}")
        out.append("")
        out.append("/* Calls the code at `label` as the MIPS ABI does: four arguments in a0-a3, the rest on the stack from")
        out.append(" * 16(sp) above the callee's argument save area. C callers pass what they pass; reading the words past")
        out.append(" * them is harmless, as cdecl leaves them on the caller's stack. */")
        out.append("static u32 Enter(u32 label, const u32 *args)")
        out.append("{")
        out.append("    u32 savedRa = R[31], savedSp, sp;")
        out.append("    int i;")
        out.append("    if (R[29] == 0) R[29] = ADDR(&sStack[0x10000 - 8]);")
        out.append("    savedSp = R[29];")
        out.append("    sp = (savedSp - 48) & ~7u;")
        out.append("    for (i = 4; i < ARG_MAX; i++) *(u32 *)PTR(sp + i * 4) = args[i];")
        out.append("    R[29] = sp;")
        out.append("    R[4] = args[0]; R[5] = args[1]; R[6] = args[2]; R[7] = args[3]; R[31] = RET_TO_C;")
        out.append("    Run(label);")
        out.append("    R[29] = savedSp;")
        out.append("    R[31] = savedRa;")
        out.append("    return R[2];")
        out.append("}")
        out.append("")
        for w in wrappers:
            storage = "" if w in self.asm.exports else "static "
            out.append(f"{storage}u32 W_{c_ident(w)}(ARGS) {{ return ENTER(CODE_BASE + {4 * self.label_index[w]}u); }}")
        out.append("")
        out.append("/* The exported routines under their own names. */")
        for w in sorted(self.asm.exports & self.code_labels):
            out.append(f"u32 {c_ident(w)}(ARGS) {{ return ENTER(CODE_BASE + {4 * self.label_index[w]}u); }}")
        out.append("")
        out.append("__attribute__((constructor)) static void InitAsmData(void)")
        out.append("{")
        out.extend(data_inits)
        out.append("    /* the emulated stack below its pointer is dead: a save state clears it (savestate.c) */")
        out.append("    SaveStateAddDeadStack(sStack, sizeof(sStack), &R[29]);")
        out.append("}")
        return "\n".join(out) + "\n"


def main():
    p = argparse.ArgumentParser()
    p.add_argument("output")
    p.add_argument("sources", nargs="+")
    p.add_argument("-I", dest="includes", action="append", default=[])
    p.add_argument("-D", dest="defines", action="append", default=[])
    args = p.parse_args()
    asm = Assembler(args.includes, args.defines)
    try:
        for src in args.sources:
            unit = c_ident(os.path.splitext(os.path.basename(src))[0])
            asm.rs, asm.section = 0, ".text"
            asm.read_file(src, unit)
        text = Translator(asm).generate()
    except AsmError as e:
        sys.exit(f"mips2c: {e}")
    with open(args.output, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


if __name__ == "__main__":
    main()
