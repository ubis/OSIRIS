#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
Host-side tests for dspboot (run via `make test` after building the target):
  1. CRC-32 / CRC-16 of the C code vs Python zlib / reference
  2. NAND 4-bit ECC unpacking vs the software RS model (rs_ti.py)
  3. image header parsing (mkimage.py output, corrupted variants)
  4. YMODEM: dspboot's C receiver vs uartboot.py's Python sender over a pty,
     including injected corruption and (if installed) lrzsz `sz` interop
  5. AIS: mkais.py output of build/<board>/dspboot.elf fed through uartboot.py's ROM
     protocol code into a simulated boot ROM that checks every CRC
  6. U-Boot environment parsing (bootslot selection, redundant copies)
  7. NAND geometry from READ ID bytes, mknandimg.py page+spare records for 512 B / 2 KiB / 4 KiB
"""
import glob
import os
import random
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.environ.get('DSPBOOT_BUILD', os.path.join(ROOT, 'build'))
BUILD = os.path.join(OUT, 'test')
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.path.insert(0, HERE)

import mkais      # noqa: E402
import mkimage    # noqa: E402
import rs_ti      # noqa: E402
import uartboot   # noqa: E402

TH = os.path.join(BUILD, 'test_host')
YRX = os.path.join(BUILD, 'ymodem_rx')
failures = 0
tmp = tempfile.mkdtemp(prefix='dspboot-test-')
rnd = random.Random(1234)


def check(name, ok, detail=''):
    global failures
    print('%-58s %s %s' % (name, 'ok' if ok else 'FAIL', detail))
    if not ok:
        failures += 1


def th(*args):
    return subprocess.check_output([TH] + list(args)).decode().strip()


def write(name, data):
    p = os.path.join(tmp, name)
    open(p, 'wb').write(data)
    return p


# 1. CRCs
for n in (0, 1, 3, 1000, 65537):
    d = bytes(rnd.getrandbits(8) for _ in range(n))
    p = write('crc.bin', d)
    check('crc32 %d bytes' % n, int(th('crc32', p), 16) == zlib.crc32(d))
    check('crc16 %d bytes' % n, int(th('crc16', p), 16) == uartboot.crc16_xmodem(d))
check('crc16 check value "123456789" = 0x31c3', uartboot.crc16_xmodem(b'123456789') == 0x31C3)

# 2. ECC unpacking
ok = True
for i in range(20):
    page = [rnd.getrandbits(8) for _ in range(512)]
    sym = rs_ti.parity(page)
    got = [int(x, 16) for x in th('unpack', rs_ti.pack(sym).hex()).split()]
    ok &= got == sym
check('nand_ecc_unpack vs RS model (20 random pages)', ok)

# 3. images
payload = bytes(rnd.getrandbits(8) for _ in range(5000))
img = mkimage.build(payload, 0xC1080000, 0xC1080000, 'u-boot')
out = th('img', write('a.dbi', img))
check('DBI image parses, CRC OK', out.startswith('rc=0 type=1') and 'payload=OK' in out, out)
bad = bytearray(img)
bad[100] ^= 1
out = th('img', write('b.dbi', bytes(bad)))
check('DBI payload corruption detected', 'payload=BAD' in out, out)
bad = bytearray(img)
bad[8] ^= 1
out = th('img', write('c.dbi', bytes(bad)))
check('DBI header corruption detected', out.startswith('rc=-74'), out)  # -EBADMSG
img2 = mkimage.build(payload, 0xC1080000, 0xC9000000, 'x')
out = th('img', write('d.dbi', img2))
check('DBI entry outside image rejected', out.startswith('rc=-34'), out)  # -ERANGE
out = th('img', write('f.bin', b'\x12\x00\x00\xea' + b'\0' * 64))
check('raw ARM binary -> no header', out.startswith('rc=-8'), out)  # -ENOEXEC
out = th('img', write('g.bin', b'\x27\x05\x19\x56' + b'\0' * 64))
check('uImage recognised and refused', out.startswith('rc=-8 type=3'), out)


# 4. YMODEM over a pty
class FdPort:
    """uartboot.Port-like wrapper around a pty master; optional corruption."""

    def __init__(self, fd, corrupt_at=None):
        self.fd = fd
        self.sent = 0
        self.corrupt_at = corrupt_at

    def write(self, data):
        data = bytearray(data)
        if self.corrupt_at is not None and self.sent <= self.corrupt_at < self.sent + len(data):
            data[self.corrupt_at - self.sent] ^= 0x40
            self.corrupt_at = None
        self.sent += len(data)
        view = memoryview(bytes(data))
        while view:
            n = os.write(self.fd, view[:256])
            view = view[n:]

    def read(self, n, timeout):
        out = b''
        end = time.monotonic() + timeout
        while len(out) < n:
            left = end - time.monotonic()
            if left <= 0:
                break
            r, _, _ = select.select([self.fd], [], [], left)
            if r:
                try:
                    out += os.read(self.fd, n - len(out))
                except OSError:
                    break
        return out


def ymodem_case(name, data, corrupt_at=None, maxlen=1 << 22):
    import pty
    m, s = pty.openpty()
    import tty
    tty.setraw(m)
    tty.setraw(s)
    outp = os.path.join(tmp, 'ym.out')
    if os.path.exists(outp):
        os.unlink(outp)
    proc = subprocess.Popen([YRX, os.ttyname(s), outp, str(maxlen)], stderr=subprocess.PIPE)
    try:
        uartboot.ymodem_send(FdPort(m, corrupt_at), data, 'test.bin', progress=False, wait_c=10)
        err_send = ''
    except IOError as e:
        err_send = str(e)
    rc = proc.wait(timeout=30)
    msg = proc.stderr.read().decode().strip()
    os.close(m)
    os.close(s)
    got = open(outp, 'rb').read() if os.path.exists(outp) else None
    check('ymodem ' + name, rc == 0 and got == data and not err_send, (err_send or msg))
    return msg


if os.path.exists(YRX):
    for n in (1, 100, 128, 129, 1024, 1025, 3000, 200000):
        ymodem_case('%d bytes' % n, bytes(rnd.getrandbits(8) for _ in range(n)))
    msg = ymodem_case('corrupted byte in block 2 -> NAK + resend',
                      bytes(rnd.getrandbits(8) for _ in range(5000)), corrupt_at=133 + 1029 + 500)
    check('ymodem corruption caused a retry', 'retries=1' in msg, msg)
    # oversize file must be refused
    import pty
    m, s = pty.openpty()
    proc = subprocess.Popen([YRX, os.ttyname(s), os.path.join(tmp, 'x'), '1000'], stderr=subprocess.PIPE)
    try:
        uartboot.ymodem_send(FdPort(m), b'\0' * 5000, 'big.bin', progress=False, wait_c=10)
        refused = False
    except IOError:
        refused = True
    proc.wait(timeout=30)
    check('ymodem oversize file refused (CAN)', refused, proc.stderr.read().decode().strip())
    os.close(m)
    os.close(s)

    sz = shutil.which('sz')
    if sz:
        import pty
        data = bytes(rnd.getrandbits(8) for _ in range(70000))
        src = write('sz.bin', data)
        m, s = pty.openpty()
        import tty
        tty.setraw(m)
        outp = os.path.join(tmp, 'sz.out')
        proc = subprocess.Popen([YRX, os.ttyname(s), outp, str(1 << 20)], stderr=subprocess.PIPE)
        szp = subprocess.Popen([sz, '--ymodem', '-k', src], stdin=m, stdout=m, stderr=subprocess.DEVNULL)
        szrc = szp.wait(timeout=60)
        rc = proc.wait(timeout=30)
        got = open(outp, 'rb').read() if os.path.exists(outp) else None
        check('ymodem interop with lrzsz sz --ymodem -k (70000 bytes)',
              rc == 0 and szrc == 0 and got == data, proc.stderr.read().decode().strip())
        os.close(m)
        os.close(s)
    else:
        print('(lrzsz sz not installed, skipping interop test)')


# 5. AIS through uartboot.py into a simulated boot ROM
class FakeRom:
    """Byte-level model of the ROM's UART AIS slave as uartboot.AisBoot drives it."""

    def __init__(self):
        self.out = bytearray()
        self.mem = {}
        self.entry = None
        self.crc_errors = 0
        self.last_crc = 0
        self.glitch_last = 0
        self.gen = self.run()
        next(self.gen)

    def write(self, data):
        for b in data:
            self.gen.send(b)

    def read(self, n, timeout):
        d, self.out = bytes(self.out[:n]), self.out[n:]
        return d

    def flush_input(self):
        pass

    def drain(self, nbytes):
        pass

    def w32(self, v):
        self.out += struct.pack('<I', v)

    def r32(self):
        b = bytearray()
        while len(b) < 4:
            b.append((yield))
        return struct.unpack('<I', b)[0]

    def run(self):
        while (yield) != 0x58:
            pass
        self.out.append(0x52)
        op = yield from self.r32()
        assert op == uartboot.OP_PING
        self.w32(uartboot.ack_of(op))
        n = yield from self.r32()
        self.w32(n)
        for _ in range(n):
            self.w32((yield from self.r32()))
        while True:
            op = yield from self.r32()
            self.w32(uartboot.ack_of(op))
            if op == uartboot.OP_SECTION_LOAD:
                addr = yield from self.r32()
                size = yield from self.r32()
                data = bytearray()
                for _ in range((size + 3) & ~3):
                    data.append((yield))
                if self.glitch_last:
                    self.glitch_last -= 1
                    data[-1] ^= 0x34
                self.mem[addr] = bytes(data[:size])
                self.last_crc = mkais.ais_crc(struct.pack('<II', addr, size) + bytes(data))
            elif op == uartboot.OP_REQUEST_CRC:
                self.w32(self.last_crc)
            elif op == uartboot.OP_ENABLE_CRC:
                pass
            elif op == uartboot.OP_JUMP_CLOSE:
                self.entry = yield from self.r32()
                while True:
                    yield
            else:
                raise AssertionError('unexpected opcode %08x' % op)


elfs = sorted(glob.glob(os.path.join(OUT, '*', 'dspboot.elf')))
elf = elfs[0] if elfs else os.path.join(OUT, 'missing.elf')
if os.path.exists(elf):
    entry, secs, _ = mkais.read_elf(elf)
    for crc in (False, True):
        ais = mkais.make_ais(entry, secs, crc)
        rom = FakeRom()
        uartboot.OPCODE_DELAY = 0
        _log, uartboot.log = uartboot.log, (lambda m: None)
        try:
            uartboot.AisBoot(rom, timeout=1).boot(ais)
            err = ''
        except Exception as e:     # noqa: BLE001
            err = repr(e)
        uartboot.log = _log
        same = all(rom.mem.get(a, b'')[:len(d)] == d for _, a, d in secs)
        check('AIS (%s) through uartboot.py into fake ROM' % ('CRC' if crc else 'no CRC'),
              not err and rom.entry == entry and same, err or 'entry 0x%08x' % (rom.entry or 0))
    # a corrupted section must fail the CRC check
    ais = bytearray(mkais.make_ais(entry, secs, True))
    ais[40] ^= 1
    rom = FakeRom()
    uartboot.log = lambda m: None
    try:
        uartboot.AisBoot(rom, timeout=1).boot(bytes(ais))
        caught = False
    except IOError as e:
        caught = 'CRC mismatch' in str(e)
    uartboot.log = _log
    check('AIS CRC catches a flipped bit', caught)
    # a section garbled on the wire is sent again
    rom = FakeRom()
    rom.glitch_last = 2
    uartboot.log = lambda m: None
    try:
        uartboot.AisBoot(rom, timeout=1).boot(mkais.make_ais(entry, secs, True))
        err = ''
    except Exception as e:     # noqa: BLE001
        err = repr(e)
    uartboot.log = _log
    same = all(rom.mem.get(a, b'')[:len(d)] == d for _, a, d in secs)
    check('AIS resends a section after a CRC mismatch', not err and rom.entry == entry and same, err)
else:
    print('(no build/<board>/dspboot.elf, skipping AIS test)')

# 6. U-Boot environment
def uenv(pairs, size=0x4000, flags=None, badcrc=False):
    """Environment area as U-Boot writes it (zero padded)."""
    hdr = 4 if flags is None else 5
    data = b''.join(b'%s=%s\0' % (k, v) for k, v in pairs) + b'\0'
    data = data.ljust(size - hdr, b'\0')
    crc = zlib.crc32(data) ^ (1 if badcrc else 0)
    return struct.pack('<I', crc) + (b'' if flags is None else bytes([flags])) + data


OTHER = [(b'bootcmd', b'nboot.jffs2 0xc0200000 0 0x454000'), (b'bootdelay', b'1'),
         (b'bootslotx', b'b'), (b'boot', b'b')]


def env_case(name, files, want_pick, want_value, want_slot, var='bootslot'):
    paths = [write('env%d' % i, f) for i, f in enumerate(files)]
    out = th('env', var, *paths)
    exp = 'pick=%d value=%s slot=%s' % (want_pick, want_value, want_slot)
    check('env: ' + name, out == exp, '' if out == exp else '(%s)' % out)


env_case('single copy, bootslot=b', [uenv(OTHER + [(b'bootslot', b'b')])], 0, 'b', 'b')
env_case('single copy, bootslot=a', [uenv([(b'bootslot', b'a')] + OTHER)], 0, 'a', 'a')
env_case('single copy, bootslot missing (prefix names only)', [uenv(OTHER)], 0, '(none)', '-')
env_case('single copy, bootslot invalid', [uenv([(b'bootslot', b'B')])], 0, 'B', '-')
env_case('single copy, bootslot too long', [uenv([(b'bootslot', b'ab')])], 0, 'ab', '-')
env_case('single copy, empty bootslot', [uenv([(b'bootslot', b'')])], 0, '', '-')
env_case('single copy, bad CRC', [uenv([(b'bootslot', b'b')], badcrc=True)], -1, '(none)', '-')
env_case('erased area', [b'\xff' * 0x4000], -1, '(none)', '-')
env_case('other variable', [uenv(OTHER)], 0, '1', '-', var='bootdelay')
A = [(b'bootslot', b'a')]
B = [(b'bootslot', b'b')]
env_case('redundant, second newer', [uenv(A, flags=3), uenv(B, flags=4)], 1, 'b', 'b')
env_case('redundant, first newer', [uenv(A, flags=9), uenv(B, flags=4)], 0, 'a', 'a')
env_case('redundant, 255 -> 0 wrap', [uenv(A, flags=255), uenv(B, flags=0)], 1, 'b', 'b')
env_case('redundant, 0 <- 255 wrap', [uenv(A, flags=0), uenv(B, flags=255)], 0, 'a', 'a')
env_case('redundant, newer copy corrupt', [uenv(A, flags=3), uenv(B, flags=4, badcrc=True)],
         0, 'a', 'a')
env_case('redundant, only second valid', [b'\xff' * 0x4000, uenv(B, flags=1)], 1, 'b', 'b')

# 7. NAND geometry and mknandimg
def geom_case(name, idhex, want):
    out = th('geom', idhex)
    check('geom: ' + name, out == want, '' if out == want else '(%s)' % out)


geom_case('20 76 ST 64 MiB small page (the 717)', '20760000',
          'page=512 spare=16 steps=1 ppb=32 block=16K blocks=4096 cyc=1+3 bbm=5')
geom_case('EC 75 Samsung 32 MiB small page', 'EC750000',
          'page=512 spare=16 steps=1 ppb=32 block=16K blocks=2048 cyc=1+2 bbm=5')
geom_case('EC F1 00 15 K9F1G08 128 MiB 2K', 'ECF10015',
          'page=2048 spare=64 steps=4 ppb=64 block=128K blocks=1024 cyc=2+2 bbm=0')
geom_case('2C DA 90 95 MT29F2G08 256 MiB 2K', '2CDA9095',
          'page=2048 spare=64 steps=4 ppb=64 block=128K blocks=2048 cyc=2+3 bbm=0')
geom_case('01 D1 80 1D S34ML01G1-like 128 MiB 2K', '01D1801D',
          'page=2048 spare=64 steps=4 ppb=64 block=128K blocks=1024 cyc=2+2 bbm=0')
geom_case('4K page, 256 KiB block, 1 GiB', 'ADD30026',
          'page=4096 spare=128 steps=8 ppb=64 block=256K blocks=4096 cyc=2+3 bbm=0')
geom_case('16-bit bus rejected', '2CDA90D5', 'rc=-95 16-bit bus')
geom_case('8 spare bytes per 512 rejected', 'ECF10011', 'rc=-95 8 spare bytes per 512 (ROM ECC needs 16)')
geom_case('512 KiB block rejected', 'ADD30036', 'rc=-95 block larger than 256 KiB')
geom_case('1 KiB page rejected', 'ECF10014', 'rc=-95 page size not 2 or 4 KiB')
geom_case('unknown device code', 'EC420015', 'rc=-19 unknown device code')

MKN = os.path.join(ROOT, 'tools', 'mknandimg.py')


def mknand(src, *extra):
    out = src + '.nand'
    subprocess.check_output([sys.executable, MKN, src, '-o', out] + list(extra))
    return open(out, 'rb').read()


payload = bytes(rnd.getrandbits(8) for _ in range(5000))
src = write('mkn.bin', payload)
img = mknand(src)
old = b''.join(payload.ljust(10 * 512, b'\xff')[i:i + 512] + b'\xff' * 6 +
               bytes(rs_ti.pack(rs_ti.parity(payload.ljust(10 * 512, b'\xff')[i:i + 512])))
               for i in range(0, 10 * 512, 512))
check('mknandimg 512: same records as the small-page-only version', img == old)
for page, block in ((2048, 128 << 10), (4096, 256 << 10)):
    img = mknand(src, '--page', str(page), '--pad-block')
    rec = page + page // 32
    data = payload.ljust(len(img) // rec * page, b'\xff')
    ok = len(img) == block // page * rec
    for p in range(len(img) // rec):
        r_ = img[p * rec:(p + 1) * rec]
        for s in range(page // 512):
            step = data[p * page + s * 512:p * page + (s + 1) * 512]
            ok &= r_[s * 512:(s + 1) * 512] == step
            ok &= r_[page + 16 * s:page + 16 * s + 6] == b'\xff' * 6
            ok &= r_[page + 16 * s + 6:page + 16 * s + 16] == bytes(rs_ti.pack(rs_ti.parity(step)))
    check('mknandimg %d: one block, ECC of step n at spare 16n+6' % page, ok)
    v = subprocess.run([sys.executable, MKN, '--verify', '--page', str(page), src + '.nand'],
                       capture_output=True)
    check('mknandimg %d: --verify accepts its own output' % page, v.returncode == 0,
          v.stdout.decode().strip())
    bad = bytearray(img)
    bad[page + 16 * 3 + 9] ^= 0x10
    open(src + '.bad', 'wb').write(bad)
    v = subprocess.run([sys.executable, MKN, '--verify', '--page', str(page), src + '.bad'],
                       capture_output=True)
    check('mknandimg %d: --verify catches a wrong ECC byte in step 3' % page, v.returncode == 1)

shutil.rmtree(tmp, ignore_errors=True)
print('\n%s' % ('ALL TESTS PASSED' if not failures else '%d FAILURE(S)' % failures))
sys.exit(1 if failures else 0)
