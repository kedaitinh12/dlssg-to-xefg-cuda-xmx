#!/usr/bin/env python3
"""Lists the kernels of a CUDA fatbin (registers, static shared memory, theoretical occupancy) and checks that it has
what the tensor core route needs: SASS for sm_75, sm_86, sm_89 and sm_120, and the 16 kernels it creates by name.
Standard library only.

usage: fatbin_info.py [--check] [--markdown] FILE...
  --check     exit 1 unless every file carries the four targets and all 16 kernels
  --markdown  print GitHub-flavoured markdown (for $GITHUB_STEP_SUMMARY) instead of plain text
"""
import hashlib
import struct
import sys

TARGETS = (75, 86, 89, 120)
# The kernels src/CudaRoute/CudaRoute.cpp creates by name (kKernels)
KERNELS = ("L_aa016078", "L_198d6bf8", "L_dcd8becf", "L_efbe3788", "L_50f0c205", "L_a3c3caa1", "L_9481d452",
           "L_06e97933", "L_3ed03daf", "L_748b0baf", "L_cb391582", "L_fc1b8a04", "L_3459485", "L_3452733",
           "L_flow16_acc17", "L_3453044")
BLOCK = 128  # threads per block, fixed by the route


def fatbin_entries(data):
    """[(kind, sm, payload)]; kind 2 is an ELF cubin, the sm number sits 0x1c bytes into the entry header."""
    magic, _version, header, size = struct.unpack_from("<IHHQ", data, 0)
    if magic != 0xBA55ED50:
        raise ValueError("not a CUDA fatbin (magic %08x)" % magic)
    out, offset = [], header
    while offset + 0x40 <= min(header + size, len(data)):
        kind, _v, entry_header, payload_size = struct.unpack_from("<HHIQ", data, offset)
        sm, = struct.unpack_from("<I", data, offset + 0x1C)
        out.append((kind, sm, data[offset + entry_header:offset + entry_header + payload_size]))
        offset += entry_header + payload_size
    return out


def cubin_kernels(elf):
    """{kernel name: (registers, static shared bytes)} of one cubin."""
    shoff, = struct.unpack_from("<Q", elf, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x3A)
    headers = [struct.unpack_from("<IIQQQQIIQQ", elf, shoff + i * shentsize) for i in range(shnum)]
    names_at = headers[shstrndx][4]
    names = [elf[names_at + h[0]:elf.index(b"\0", names_at + h[0])].decode() for h in headers]
    section = dict(zip(names, headers))

    symtab = section[".symtab"]
    strtab = headers[symtab[6]]
    symbols = []
    for i in range(symtab[5] // 24):
        name_at, = struct.unpack_from("<I", elf, symtab[4] + 24 * i)
        symbols.append(elf[strtab[4] + name_at:elf.index(b"\0", strtab[4] + name_at)].decode())

    registers = {}
    info = section.get(".nv.info")
    if info:
        offset = info[4]
        while offset + 4 <= info[4] + info[5]:
            fmt, attr, size = elf[offset], elf[offset + 1], struct.unpack_from("<H", elf, offset + 2)[0]
            if fmt == 4:                                    # EIFMT_SVAL
                if attr == 0x2F and size >= 8:              # EIATTR_REGCOUNT: symbol index, register count
                    index, count = struct.unpack_from("<II", elf, offset + 4)
                    registers[symbols[index]] = count
                offset += 4 + size
            else:
                offset += 4

    kernels = {}
    for name in names:
        if name.startswith(".text."):
            kernel = name[len(".text."):]
            shared = section.get(".nv.shared." + kernel)
            kernels[kernel] = (registers.get(kernel), shared[5] if shared else 0)
    return kernels


def blocks_per_sm_86(registers, shared):
    """Theoretical blocks of 128 threads per SM on sm_86: 64K registers, 100 KB shared memory (+1 KB reserved per
    block), 48 warps, 16 blocks."""
    per_warp = -(-registers * 32 // 256) * 256
    by_registers = 65536 // (per_warp * 4)
    by_shared = 102400 // (shared + 1024)
    return min(by_registers, by_shared, 48 // (BLOCK // 32), 16)


def inspect(path):
    data = open(path, "rb").read()
    per_target = {sm: cubin_kernels(payload) for kind, sm, payload in fatbin_entries(data) if kind == 2}
    problems = []
    for sm in TARGETS:
        if sm not in per_target:
            problems.append("no SASS for sm_%d" % sm)
            continue
        missing = [k for k in KERNELS if k not in per_target[sm]]
        if missing:
            problems.append("sm_%d lacks %s" % (sm, ", ".join(missing)))
    return data, per_target, problems


def report(path, markdown):
    data, per_target, problems = inspect(path)
    digest = hashlib.sha256(data).hexdigest()
    sm86 = per_target.get(86, {})
    lines = []
    head = "%s: %d bytes, SHA-256 %s, targets %s" % (path, len(data), digest,
                                                     ", ".join("sm_%d" % sm for sm in sorted(per_target)))
    lines.append(("### `%s`\n\n%d bytes, SHA-256 `%s`, targets %s\n" % (
        path, len(data), digest, ", ".join("sm_%d" % sm for sm in sorted(per_target)))) if markdown else head)
    rows = []
    for kernel in KERNELS:
        registers = [per_target.get(sm, {}).get(kernel, (None, 0))[0] for sm in TARGETS]
        shared = sm86.get(kernel, (None, 0))[1]
        reg86 = sm86.get(kernel, (None, 0))[0]
        blocks = blocks_per_sm_86(reg86, shared) if reg86 else None
        rows.append((kernel, registers, shared, blocks))
    if markdown:
        lines.append("| kernel | registers sm_75 / 86 / 89 / 120 | shared | blocks per SM (sm_86) |")
        lines.append("|---|---|---|---|")
        for kernel, registers, shared, blocks in rows:
            lines.append("| `%s` | %s | %.1f KB | %s |" % (kernel, " / ".join(str(r) for r in registers), shared / 1024,
                                                           "%d (%d of 48 warps)" % (blocks, blocks * 4) if blocks else "?"))
        lines.append("")
    else:
        lines.append("  %-16s %-24s %8s  %s" % ("kernel", "registers 75/86/89/120", "shared", "blocks/SM sm_86"))
        for kernel, registers, shared, blocks in rows:
            lines.append("  %-16s %-24s %6.1f K  %s" % (kernel, "/".join(str(r) for r in registers), shared / 1024,
                                                      "%d (%d/48 warps)" % (blocks, blocks * 4) if blocks else "?"))
    for problem in problems:
        lines.append(("**Problem:** " if markdown else "PROBLEM: ") + problem)
    return "\n".join(lines), problems


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = {a for a in sys.argv[1:] if a.startswith("--")}
    if not args or flags - {"--check", "--markdown"}:
        sys.exit(__doc__)
    failed = False
    for path in args:
        try:
            text, problems = report(path, "--markdown" in flags)
        except (OSError, ValueError, KeyError, struct.error) as error:
            text, problems = "%s: unreadable (%s)" % (path, error), ["unreadable"]
        print(text)
        failed |= bool(problems)
    if failed and "--check" in flags:
        sys.exit(1)


if __name__ == "__main__":
    main()
