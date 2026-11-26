/* The I/O registers (mmio.h). The 64 KB from 0x1F800000 are mapped read-only at the console's address and
 * writable at another, both views of the same memory: the game's code reads registers at full speed, as
 * plain memory the emulated devices keep current, and every write faults into the handler here, which
 * decodes the x86 store, gives the value to the device (spu.c) and resumes after the instruction.
 *
 * The decoder takes the stores a compiler emits for volatile register writes: mov from a register or an
 * immediate, of 8, 16 or 32 bits, by ModRM or absolute address, and the read-modify-write forms of add,
 * or, and, sub and xor. */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#define _GNU_SOURCE
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include "host.h"
#include "mmio.h"
#include "spu.h"
#include "mdec.h"

#define IO_BASE 0x1F800000u
#define IO_SIZE 0x10000u

static uint8_t *sWritable;

void *MmioRegister(uint32_t address)
{
    return sWritable + (address - IO_BASE);
}

/* The faulting thread's general registers in the instruction set's order: eax, ecx, edx, ebx, esp, ebp, esi,
 * edi. */
static uint32_t Register32(const uint32_t *regs, int index)
{
    return regs[index & 7];
}

static uint32_t Register8(const uint32_t *regs, int index)
{
    /* al, cl, dl, bl, then ah, ch, dh, bh */
    return index < 4 ? Register32(regs, index) & 0xFF : (Register32(regs, index - 4) >> 8) & 0xFF;
}

/* The length of a ModRM memory operand's bytes from the ModRM byte on. */
static int ModRmLength(const uint8_t *p)
{
    int mod = p[0] >> 6, rm = p[0] & 7, len = 1;
    if (mod == 3) {
        return 1;
    }
    if (rm == 4) {
        int base = p[1] & 7;
        len++;
        if (mod == 0 && base == 5) {
            len += 4;
        }
    } else if (mod == 0 && rm == 5) {
        len += 4;
    }
    if (mod == 1) {
        len += 1;
    } else if (mod == 2) {
        len += 4;
    }
    return len;
}

static uint32_t Alu(int op, uint32_t a, uint32_t b)
{
    switch (op) {
    case 0: return a + b;
    case 1: return a | b;
    case 4: return a & b;
    case 5: return a - b;
    case 6: return a ^ b;
    default: return b;
    }
}

/* Decodes the store at ip: its length, width in bytes and the value it leaves given the old one. */
static int DecodeStore(const uint8_t *ip, const uint32_t *regs, uint32_t old, uint32_t *value, int *width)
{
    const uint8_t *p = ip;
    int opsize16 = 0, op, n, reg;
    uint32_t imm;

    while (*p == 0x66 || *p == 0x2E || *p == 0x3E || *p == 0x26) {
        opsize16 |= *p == 0x66;
        p++;
    }
    op = *p++;
    *width = opsize16 ? 2 : 4;
    switch (op) {
    case 0xA2:
        *width = 1;
        *value = regs[0] & 0xFF;
        return (int)(p - ip) + 4;
    case 0xA3:
        *value = regs[0];
        return (int)(p - ip) + 4;
    case 0x88:
    case 0x00: case 0x08: case 0x20: case 0x28: case 0x30:
        *width = 1;
        /* fall through */
    case 0x89:
    case 0x01: case 0x09: case 0x21: case 0x29: case 0x31:
        reg = (p[0] >> 3) & 7;
        n = ModRmLength(p);
        imm = *width == 1 ? Register8(regs, reg) : Register32(regs, reg);
        *value = (op == 0x88 || op == 0x89) ? imm : Alu(op >> 3, old, imm);
        return (int)(p - ip) + n;
    case 0xC6:
        *width = 1;
        n = ModRmLength(p);
        *value = p[n];
        return (int)(p - ip) + n + 1;
    case 0xC7:
        n = ModRmLength(p);
        *value = opsize16 ? *(const uint16_t *)(p + n) : *(const uint32_t *)(p + n);
        return (int)(p - ip) + n + (opsize16 ? 2 : 4);
    case 0x80:
    case 0x81:
    case 0x83: {
        int size = op == 0x80 ? 1 : (op == 0x83 ? 1 : (opsize16 ? 2 : 4));
        if (op == 0x80) {
            *width = 1;
        }
        reg = (p[0] >> 3) & 7;
        n = ModRmLength(p);
        if (size == 1) {
            imm = op == 0x83 ? (uint32_t)(int32_t)(int8_t)p[n] : p[n];
        } else if (size == 2) {
            imm = *(const uint16_t *)(p + n);
        } else {
            imm = *(const uint32_t *)(p + n);
        }
        *value = Alu(reg, old, imm);
        return (int)(p - ip) + n + size;
    }
    default:
        return 0;
    }
}

static void WriteRegister(uint32_t address, uint32_t value, int width)
{
    if (address >= 0x1F801C00 && address < 0x1F801E00) {
        /* The SPU's registers are 16 bits: a wider store writes two. */
        if (width == 4) {
            WriteRegister(address, value & 0xFFFF, 2);
            WriteRegister(address + 2, value >> 16, 2);
            return;
        }
        if (width == 1) {
            uint16_t *reg = (uint16_t *)MmioRegister(address & ~1u);
            value = (address & 1) ? ((*reg & 0xFF) | value << 8) : ((*reg & 0xFF00) | value);
        }
        SpuWriteRegister((address & ~1u) - 0x1F801C00, (uint16_t)value);
        return;
    }
    if (width == 1) {
        *(uint8_t *)MmioRegister(address) = (uint8_t)value;
    } else if (width == 2) {
        *(uint16_t *)MmioRegister(address) = (uint16_t)value;
    } else {
        *(uint32_t *)MmioRegister(address) = value;
    }
    if (address >= 0x1F8010C0 && address < 0x1F8010D0) {
        SpuDmaRegisterWritten(address);
    }
    if (address >= 0x1F801080 && address < 0x1F8010A0) {
        MdecDmaWritten(address & ~3u);
    }
    if (address >= 0x1F801820 && address < 0x1F801828) {
        MdecRegisterWritten(address & ~3u, *(uint32_t *)MmioRegister(address & ~3u));
    }
}

/* A faulting write to the register at `address` by the instruction at `eip`: carried out, and the length of
 * the instruction to step over returned. */
static int EmulateStore(uint32_t address, uint32_t eip, const uint32_t *regs)
{
    uint32_t old, value;
    int length, width;
    old = *(uint32_t *)MmioRegister(address & ~3u) >> ((address & 3) * 8);
    length = DecodeStore((const uint8_t *)(uintptr_t)eip, regs, old, &value, &width);
    if (length == 0) {
        const uint8_t *ip = (const uint8_t *)(uintptr_t)eip;
        HostFatal("a register write at %08lx by an instruction at %08lx the port does not decode (%02x %02x %02x %02x)",
                  (unsigned long)address, (unsigned long)eip, ip[0], ip[1], ip[2], ip[3]);
    }
    if (width < 4) {
        value &= width == 1 ? 0xFF : 0xFFFF;
    }
    WriteRegister(address, value, width);
    return length;
}

#ifdef _WIN32
static LONG WINAPI MmioHandler(EXCEPTION_POINTERS *info)
{
    EXCEPTION_RECORD *rec = info->ExceptionRecord;
    CONTEXT *ctx = info->ContextRecord;
    uint32_t address, regs[8];

    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->ExceptionInformation[0] != 1) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    address = (uint32_t)rec->ExceptionInformation[1];
    if (address < IO_BASE || address >= IO_BASE + IO_SIZE) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    regs[0] = ctx->Eax, regs[1] = ctx->Ecx, regs[2] = ctx->Edx, regs[3] = ctx->Ebx;
    regs[4] = ctx->Esp, regs[5] = ctx->Ebp, regs[6] = ctx->Esi, regs[7] = ctx->Edi;
    ctx->Eip += EmulateStore(address, ctx->Eip, regs);
    return EXCEPTION_CONTINUE_EXECUTION;
}

int MmioInit(void)
{
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, IO_SIZE, NULL);
    void *readOnly;
    if (!section) {
        return 0;
    }
    readOnly = MapViewOfFileEx(section, FILE_MAP_READ, 0, 0, IO_SIZE, (void *)(uintptr_t)IO_BASE);
    sWritable = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, IO_SIZE);
    if (readOnly != (void *)(uintptr_t)IO_BASE || !sWritable) {
        return 0;
    }
    AddVectoredExceptionHandler(1, MmioHandler);
    return 1;
}
#else
static struct sigaction sPreviousSegv; /* the crash report's (host.c), for every other fault */

static void MmioHandler(int sig, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    greg_t *g = uc->uc_mcontext.gregs;
    uint32_t address = (uint32_t)(uintptr_t)info->si_addr, regs[8];

    /* (the page fault's error code has bit 1 set for a write) */
    if (address >= IO_BASE && address < IO_BASE + IO_SIZE && (g[REG_ERR] & 2)) {
        regs[0] = g[REG_EAX], regs[1] = g[REG_ECX], regs[2] = g[REG_EDX], regs[3] = g[REG_EBX];
        regs[4] = g[REG_ESP], regs[5] = g[REG_EBP], regs[6] = g[REG_ESI], regs[7] = g[REG_EDI];
        g[REG_EIP] += EmulateStore(address, (uint32_t)g[REG_EIP], regs);
        return;
    }
    if (sPreviousSegv.sa_flags & SA_SIGINFO) {
        sPreviousSegv.sa_sigaction(sig, info, context);
    } else if (sPreviousSegv.sa_handler != SIG_DFL && sPreviousSegv.sa_handler != SIG_IGN) {
        sPreviousSegv.sa_handler(sig);
    } else {
        signal(sig, SIG_DFL); /* the fault happens again, unhandled */
    }
}

int MmioInit(void)
{
    /* Two views of one piece of shared memory: read-only at the console's address, writable elsewhere. */
    int fd = memfd_create("dojo-io", 0);
    struct sigaction action;
    void *readOnly;
    if (fd < 0 || ftruncate(fd, IO_SIZE) != 0) {
        return 0;
    }
    readOnly = mmap((void *)(uintptr_t)IO_BASE, IO_SIZE, PROT_READ, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
    sWritable = mmap(NULL, IO_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (readOnly != (void *)(uintptr_t)IO_BASE || sWritable == MAP_FAILED) {
        return 0;
    }
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = MmioHandler;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, &sPreviousSegv);
    return 1;
}
#endif
