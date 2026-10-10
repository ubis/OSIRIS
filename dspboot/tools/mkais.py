#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
mkais.py - turn a little-endian C6000 ELF into an AIS boot image for the
OMAP-L137 / DA830 boot ROM (UART or NAND boot).

    mkais.py [--crc] dspboot.elf -o dspboot.ais
    mkais.py --info dspboot.elf

Output: magic 0x41504954, [ENABLE_CRC], per loadable section
SECTION_LOAD(addr, size, data) [+ REQUEST_CRC(crc, seek)], JUMP_CLOSE(entry).

The CRC is the one TI's AISGen uses for this device (AISGen_OMAP-L137.cs:
CRC32 poly 0x04C11DB7, init 0, no final XOR, not reflected, bitwise variant
that feeds each 32-bit word MSB-first, computed over addr, size and data).
It reproduces every REQUEST_CRC value of a production, ROM-accepted AIS image.
No third-party modules needed.
"""
import argparse
import struct
import sys

AIS_MAGIC = 0x41504954
OP_SECTION_LOAD = 0x58535901
OP_REQUEST_CRC = 0x58535902
OP_ENABLE_CRC = 0x58535903
OP_JUMP_CLOSE = 0x58535906

SHT_PROGBITS = 1
SHF_ALLOC = 0x2
EM_TI_C6000 = 140

# L2 range the boot ROM may load into, used only for a sanity warning
L2_LO, L2_HI = 0x11800000, 0x11840000


def _crc_table():
    t = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
        t.append(c)
    return t


_TABLE = _crc_table()


def ais_crc(data, crc=0):
    """TI AISGen BitWiseCRC (non-reflected), len(data) must be a multiple of 4."""
    assert len(data) % 4 == 0
    for i in range(0, len(data), 4):
        for b in data[i:i + 4][::-1]:
            crc = (((crc << 8) & 0xFFFFFFFF) | b) ^ _TABLE[crc >> 24]
    return crc


def read_elf(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF' or d[4] != 1 or d[5] != 1:
        raise SystemExit('%s: not a 32-bit little-endian ELF' % path)
    (e_type, e_machine, _, e_entry, _, e_shoff, _, _, _, _, e_shentsize, e_shnum,
     e_shstrndx) = struct.unpack_from('<HHIIIIIHHHHHH', d, 16)
    if e_machine != EM_TI_C6000:
        raise SystemExit('%s: not a C6000 ELF (machine %d)' % (path, e_machine))
    shdrs = [struct.unpack_from('<IIIIIIIIII', d, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    strtab = shdrs[e_shstrndx]
    names = d[strtab[4]:strtab[4] + strtab[5]]

    def name(off):
        return names[off:names.index(b'\0', off)].decode()

    secs = []
    for sh in shdrs:
        nm, typ, flags, addr, off, size = sh[:6]
        if typ == SHT_PROGBITS and flags & SHF_ALLOC and size:
            secs.append((name(nm), addr, d[off:off + size]))
    nobits = [(name(sh[0]), sh[3], sh[5]) for sh in shdrs if sh[1] == 8 and sh[2] & SHF_ALLOC]
    return e_entry, sorted(secs, key=lambda s: s[1]), nobits


def make_ais(entry, secs, crc=True):
    out = [struct.pack('<I', AIS_MAGIC)]
    if crc:
        out.append(struct.pack('<I', OP_ENABLE_CRC))
    for _, addr, data in secs:
        data = data + b'\0' * (-len(data) % 4)
        out.append(struct.pack('<III', OP_SECTION_LOAD, addr, len(data)))
        out.append(data)
        if crc:
            c = ais_crc(struct.pack('<II', addr, len(data)) + data)
            # seek back over: SECTION_LOAD hdr (12) + data + REQUEST_CRC (12)
            out.append(struct.pack('<IIi', OP_REQUEST_CRC, c, -(len(data) + 24)))
    out.append(struct.pack('<II', OP_JUMP_CLOSE, entry))
    return b''.join(out)


def main():
    ap = argparse.ArgumentParser(description='C6000 ELF -> OMAP-L137 AIS')
    ap.add_argument('elf')
    ap.add_argument('-o', '--output')
    ap.add_argument('--crc', action='store_true', help='add per-section REQUEST_CRC')
    ap.add_argument('--info', action='store_true', help='print the load map')
    a = ap.parse_args()

    entry, secs, nobits = read_elf(a.elf)
    lo = min(s[1] for s in secs)
    hi = max([s[1] + len(s[2]) for s in secs] + [s[1] + s[2] for s in nobits])
    if a.info or not a.output:
        print('entry 0x%08x' % entry)
        for nm, addr, data in secs:
            print('  load %-10s 0x%08x %6d bytes' % (nm, addr, len(data)))
        for nm, addr, size in nobits:
            print('  zero %-10s 0x%08x %6d bytes' % (nm, addr, size))
        print('L2 footprint 0x%08x..0x%08x = %d bytes (%.1f KiB)' % (lo, hi, hi - lo, (hi - lo) / 1024))
    if lo < L2_LO or hi > L2_HI:
        print('WARNING: image is outside DSP L2 RAM', file=sys.stderr)
    if a.output:
        ais = make_ais(entry, secs, a.crc)
        open(a.output, 'wb').write(ais)
        print('wrote %s: %d bytes, %d sections%s, entry 0x%08x'
              % (a.output, len(ais), len(secs), ' + CRC' if a.crc else '', entry))


if __name__ == '__main__':
    main()
