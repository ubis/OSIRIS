# SPDX-License-Identifier: BSD-2-Clause
"""
Software model of the DA830/OMAP-L137 EMIFA 4-bit NAND ECC (as used by the boot ROM):
Reed-Solomon over GF(2^10), primitive polynomial x^10 + x^3 + 1, generator roots
alpha^1..alpha^8, data bytes as 10-bit symbols, 8 parity symbols stored lowest degree
first as a little-endian 80-bit field in OOB bytes 6..15.
This model reproduced the stored ECC of every page of a production board's
second-stage partition (512/512 pages).
"""
_EXP = [0] * 2048
_LOG = [0] * 1024
_x = 1
for _i in range(1023):
    _EXP[_i] = _x
    _LOG[_x] = _i
    _x <<= 1
    if _x & 0x400:
        _x ^= 0x409
for _i in range(1023, 2048):
    _EXP[_i] = _EXP[_i - 1023]


def _mul(a, b):
    return 0 if a == 0 or b == 0 else _EXP[_LOG[a] + _LOG[b]]


_G = [1]
for _i in range(8):
    _r = _EXP[1 + _i]
    _ng = [0] * (len(_G) + 1)
    for _j, _c in enumerate(_G):
        _ng[_j] ^= _c
        _ng[_j + 1] ^= _mul(_c, _r)
    _G = _ng


def parity(data):
    par = [0] * 8
    for s in data:
        fb = s ^ par[0]
        par = par[1:] + [0]
        if fb:
            for j in range(8):
                par[j] ^= _mul(fb, _G[j + 1])
    return par[::-1]


def pack(sym8):
    v = 0
    for i, s in enumerate(sym8):
        v |= s << (10 * i)
    return v.to_bytes(10, 'little')
