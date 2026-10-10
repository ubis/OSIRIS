#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
List the commands of a DA830 / OMAP-L137 AIS image, e.g. the stock first
stage of a board (block 1 of its NAND, ECC-corrected data only), and print
the board file values its boot ROM calls imply:

    aisdump.py block1.bin

ROM function 0 (PLL0) and 2 (EMIFB SDRAM) carry the clock and SDRAM setup,
a SET of 0x68000014 the NAND (EMIFA CS3) timing.
"""
import struct
import sys

MAGIC = 0x41504954
OPS = {
    0x58535901: 'SECTION_LOAD', 0x58535902: 'REQUEST_CRC', 0x58535903: 'ENABLE_CRC',
    0x58535904: 'DISABLE_CRC', 0x58535905: 'JUMP', 0x58535906: 'JUMP_CLOSE',
    0x58535907: 'SET', 0x58535909: 'COMPRESSED_LOAD', 0x5853590A: 'SECTION_FILL',
    0x5853590D: 'FUNCTION_EXECUTE',
}
FUNCS = {0: 'PLL0 config', 2: 'EMIFB SDRAM config'}


def board_hints(func, args, hints):
    if func == 0 and len(args) >= 2:
        a, b = args
        hints.append('BOARD_PLLM          %d' % ((a >> 24) & 0x1F))
        hints.append('BOARD_POSTDIV       %d' % ((a >> 16) & 0x1F))
        hints.append('BOARD_PLLDIV3       %d' % ((a >> 8) & 0x1F))
        hints.append('BOARD_PLLDIV5       %d' % (a & 0x1F))
        hints.append('BOARD_PLLDIV7       %d' % ((b >> 16) & 0x1F))
    elif func == 2 and len(args) >= 4:
        for name, v in zip(('SDCR', 'SDTIMR', 'SDTIMR2', 'SDRCR'), args):
            hints.append('BOARD_%-14s0x%08Xu' % (name, v))


def dump(d):
    i = d.find(struct.pack('<I', MAGIC))
    if i < 0:
        print('no AIS magic found')
        return 1
    print('AIS at 0x%x' % i)
    p, hints = i + 4, []

    def w():
        nonlocal p
        v = struct.unpack_from('<I', d, p)[0]
        p += 4
        return v

    while p <= len(d) - 4:
        at, op = p, w()
        name = OPS.get(op, '?')
        if op in (0x58535901, 0x58535909):
            addr, n = w(), w()
            print('%06x %s 0x%08x, %u bytes' % (at, name, addr, n))
            p += (n + 3) & ~3
        elif op == 0x5853590A:
            addr, n, _, v = w(), w(), w(), w()
            print('%06x %s 0x%08x, %u bytes of 0x%08x' % (at, name, addr, n, v))
        elif op == 0x5853590D:
            f = w()
            args = [w() for _ in range(f >> 16)]
            func = f & 0xFFFF
            print('%06x %s %d (%s): %s' % (at, name, func, FUNCS.get(func, 'unknown'),
                                          ' '.join('%08x' % a for a in args)))
            board_hints(func, args, hints)
        elif op == 0x58535907:
            t, addr, v, sleep = w(), w(), w(), w()
            print('%06x %s type %d: 0x%08x = 0x%08x, sleep %u' % (at, name, t, addr, v, sleep))
            if addr == 0x68000014:
                hints.append('BOARD_NAND_CE3CFG   0x%08Xu' % v)
        elif op == 0x58535902:
            crc, skip = w(), w()
            print('%06x %s %08x, seek %d' % (at, name, crc, struct.unpack('<i', struct.pack('<I', skip))[0]))
        elif op in (0x58535903, 0x58535904):
            print('%06x %s' % (at, name))
        elif op in (0x58535905, 0x58535906):
            print('%06x %s 0x%08x' % (at, name, w()))
            if op == 0x58535906:
                break
        else:
            print('%06x unknown opcode %08x, stopping' % (at, op))
            break
    if hints:
        print('\nboard file values:')
        for h in hints:
            print('#define ' + h)
    return 0


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip())
    sys.exit(dump(open(sys.argv[1], 'rb').read()))
