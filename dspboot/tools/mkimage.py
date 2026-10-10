#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
mkimage.py - wrap an ARM binary (e.g. u-boot.bin) in a dspboot image header.

    mkimage.py u-boot.bin u-boot.dbi [--load 0xC1080000] [--entry 0xC1080000] [--name NAME]
    mkimage.py --show u-boot.dbi

Header (64 bytes, little endian), see src/image.h:
  magic 'DBI1', hdr_size 64, size, load, entry, crc32(payload), flags 0,
  name[32], crc32(header bytes 0..59). CRC-32 = zlib.crc32.
"""
import argparse
import struct
import zlib

DBI_MAGIC = 0x31494244
HDR = struct.Struct('<IIIIIII32s')


def build(payload, load, entry, name):
    h = HDR.pack(DBI_MAGIC, 64, len(payload), load, entry, zlib.crc32(payload), 0,
                 name.encode()[:32].ljust(32, b'\0'))
    return h + struct.pack('<I', zlib.crc32(h)) + payload


def show(data):
    magic, hsz, size, load, entry, dcrc, flags, name = HDR.unpack_from(data)
    hcrc = struct.unpack_from('<I', data, 60)[0]
    ok_h = zlib.crc32(data[:60]) == hcrc
    ok_d = zlib.crc32(data[64:64 + size]) == dcrc
    print('magic %08x %s, size %d, load 0x%08x, entry 0x%08x, name %r'
          % (magic, 'OK' if magic == DBI_MAGIC else 'BAD', size, load, entry, name.rstrip(b'\0').decode()))
    print('header CRC %s, payload CRC %08x %s' % ('OK' if ok_h else 'BAD', dcrc, 'OK' if ok_d else 'BAD'))
    return ok_h and ok_d and magic == DBI_MAGIC


def main():
    ap = argparse.ArgumentParser(description='make a dspboot (DBI1) image')
    ap.add_argument('input')
    ap.add_argument('output', nargs='?')
    ap.add_argument('--load', type=lambda x: int(x, 0), default=0xC1080000)
    ap.add_argument('--entry', type=lambda x: int(x, 0))
    ap.add_argument('--name', default='u-boot')
    ap.add_argument('--show', action='store_true')
    a = ap.parse_args()
    data = open(a.input, 'rb').read()
    if a.show:
        raise SystemExit(0 if show(data) else 1)
    if not a.output:
        ap.error('output file required')
    img = build(data, a.load, a.entry if a.entry is not None else a.load, a.name)
    open(a.output, 'wb').write(img)
    show(img)


if __name__ == '__main__':
    main()
