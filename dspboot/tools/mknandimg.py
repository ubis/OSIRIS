#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
Wrap a binary into NAND page+spare records with the DA830 boot ROM's 4-bit ECC,
ready for `nand writeraw` / nandrestore.py. Each 512-byte step of a page gets 16
spare bytes: 6 bytes 0xFF (bad block marker area) + its 10 ECC bytes, so the spare
area of a 2 KiB page holds the ECC at 6-15, 22-31, 38-47 and 54-63.

    mknandimg.py dspboot.ais -o dspboot.nand [--page 2048] [--pad-block]
    mknandimg.py --verify nanddump.oob [--page 2048]   # recompute ECC of a raw dump and compare
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tests'))
import rs_ti                                # noqa: E402

STEP, STEP_SPARE = 512, 16
DEFAULT_BLOCK = {512: 16 << 10, 2048: 128 << 10, 4096: 256 << 10}


def spare_for(page):
    # ECC is written for blank (all 0xFF) pages too, as the vendor images have it
    return b''.join(b'\xff' * 6 + bytes(rs_ti.pack(rs_ti.parity(page[i:i + STEP])))
                    for i in range(0, len(page), STEP))


def records(data, page_sz, block=None):
    """data as page+spare records, padded with 0xFF to whole pages, or to a whole block"""
    npages = -(-len(data) // page_sz)
    if block:
        ppb = block // page_sz
        npages = -(-npages // ppb) * ppb
    data = data.ljust(npages * page_sz, b'\xff')
    return b''.join(data[i * page_sz:(i + 1) * page_sz] + spare_for(data[i * page_sz:(i + 1) * page_sz])
                    for i in range(npages))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('input')
    ap.add_argument('-o', '--output')
    ap.add_argument('--page', type=int, default=512, choices=sorted(DEFAULT_BLOCK),
                    help='page size in bytes (default 512)')
    ap.add_argument('--block', type=lambda s: int(s, 0),
                    help='block size in bytes for --pad-block (default 16/128/256 KiB by page size)')
    ap.add_argument('--pad-block', action='store_true', help='pad to a whole block')
    ap.add_argument('--verify', action='store_true', help='input is a raw page+spare dump')
    args = ap.parse_args()
    page_sz = args.page
    spare_sz = page_sz // STEP * STEP_SPARE
    rec = page_sz + spare_sz
    data = open(args.input, 'rb').read()

    if args.verify:
        good = bad = erased = 0
        for i in range(len(data) // rec):
            page, spare = data[i * rec:i * rec + page_sz], data[i * rec + page_sz:(i + 1) * rec]
            want = spare_for(page)
            if page == b'\xff' * page_sz and spare == b'\xff' * spare_sz:
                erased += 1
            elif all(want[s + 6:s + 16] == spare[s + 6:s + 16] for s in range(0, spare_sz, STEP_SPARE)):
                good += 1
            else:
                bad += 1
        print('%s: %d pages match, %d differ, %d erased' % (args.input, good, bad, erased))
        return 0 if bad == 0 else 1

    if not args.output:
        ap.error('-o is required')
    block = args.block or DEFAULT_BLOCK[page_sz]
    if block % page_sz:
        ap.error('block size must be a multiple of the page size')
    out = records(data, page_sz, block if args.pad_block else None)
    open(args.output, 'wb').write(out)
    print('%s: %d pages of %d+%d bytes, %d bytes' % (args.output, len(out) // rec, page_sz, spare_sz, len(out)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
