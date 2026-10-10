#!/usr/bin/env python3
"""
nandrestore: byte-exact NAND restore through dspboot, no Linux needed.

UART-boots dspboot, then for every IMAGE@OFFSET (a nanddump-with-OOB file or mknandimg.py output)
does `loady` -> `nand writeraw` -> `nand crcraw` and compares the CRC32 with the local file.
Records are page + spare: 528 bytes for 512-byte pages, 2112 for 2 KiB (--page 2048), 4224 for 4 KiB.

    nandrestore.py -p /dev/cu.usbserial-XXX --ais dspboot.ais mtd1.oob@0x4000 mtd3.oob@0x54000 ...
"""

import argparse
import os
import re
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uartboot as ub                       # noqa: E402

LOAD_ADDR = 0xC1000000


_pending = ''                               # received text not consumed by an earlier match


def expect(port, pattern, timeout):
    """Echo everything; return the regex match once 'pattern' shows up in the received text.
    Text after the match is kept for the next call."""
    global _pending
    rx = re.compile(pattern)
    end = time.monotonic() + timeout
    while True:
        m = rx.search(_pending)
        if m:
            _pending = _pending[m.end():]
            return m
        if time.monotonic() > end:
            raise IOError('timeout waiting for %r' % pattern)
        b = port.read(256, 0.2)
        if b:
            s = b.decode(errors='replace')
            sys.stdout.write(s)
            sys.stdout.flush()
            _pending = (_pending + s)[-8192:]


def _flush_pending():
    global _pending
    _pending = ''


def cmd(port, line):
    port.write(line.encode() + b'\r')


def main():
    ap = argparse.ArgumentParser(description='byte-exact NAND restore via dspboot')
    ap.add_argument('-p', '--port', required=True)
    ap.add_argument('-b', '--baud', type=int, default=96000)
    ap.add_argument('--ais', required=True)
    ap.add_argument('--skip-ais', action='store_true', help='dspboot is already at its prompt')
    ap.add_argument('--page', type=int, default=512, choices=(512, 2048, 4096),
                    help='NAND page size (default 512)')
    ap.add_argument('images', nargs='+', help='FILE@OFFSET')
    args = ap.parse_args()

    rec = args.page + args.page // 32
    jobs = []
    for spec in args.images:
        path, ofs = spec.rsplit('@', 1)
        data = open(path, 'rb').read()
        if len(data) % rec:
            sys.exit('%s: size %d is not a multiple of %d (page+spare records)' % (path, len(data), rec))
        jobs.append((path, int(ofs, 0), data))

    if args.skip_ais:
        port = ub.Port(args.port, args.baud)
    else:
        ub.log('power on / reset the board in UART boot mode now')
        while True:                          # the adapter may vanish while the board power-cycles
            try:
                port = ub.Port(args.port, args.baud)
                ub.AisBoot(port).boot(open(args.ais, 'rb').read())
                break
            except OSError as e:
                ub.log('serial port: %s - waiting for it to come back' % e)
                try:
                    port.close()
                except Exception:
                    pass
                ub.wait_for_port(args.port)
    port.set_baud(115200)
    if args.skip_ais:
        cmd(port, '')
    else:
        expect(port, r'(Hit|Press) any key', 30)
        port.write(b' ')
    expect(port, r'dspboot> $', 30)

    results = []
    for path, ofs, data in jobs:
        name = os.path.basename(path)
        ub.log('\n=== %s -> NAND 0x%08x (%d bytes, %d pages)' % (name, ofs, len(data), len(data) // rec))
        cmd(port, 'loady 0x%08x' % LOAD_ADDR)
        expect(port, r'ymodem: ready[^\n]*\n', 15)
        _flush_pending()
        ub.ymodem_send(port, data, name)
        expect(port, r'ymodem: received[^\n]*\n', 15)
        expect(port, r'dspboot> $', 10)
        cmd(port, 'nand writeraw 0x%08x 0x%x %d' % (LOAD_ADDR, ofs, len(data)))
        expect(port, r'(writeraw: done[^\n]*\n|stopped|nothing written|must be)', 600)
        expect(port, r'dspboot> $', 10)
        cmd(port, 'nand crcraw 0x%x %d' % (ofs, len(data)))
        m = expect(port, r'CRC32 ([0-9a-f]{8})', 120)
        got, want = int(m.group(1), 16), zlib.crc32(data) & 0xFFFFFFFF
        ok = got == want
        results.append((name, ofs, ok, got, want))
        ub.log('\n%s: NAND CRC32 %08x, file CRC32 %08x -> %s' % (name, got, want, 'MATCH' if ok else 'MISMATCH'))
        expect(port, r'dspboot> $', 10)

    ub.log('\n=== summary')
    for name, ofs, ok, got, want in results:
        ub.log('  %-14s 0x%08x  %s' % (name, ofs, 'OK' if ok else 'MISMATCH (nand %08x file %08x)' % (got, want)))
    sys.exit(0 if all(r[2] for r in results) else 1)


if __name__ == '__main__':
    main()
