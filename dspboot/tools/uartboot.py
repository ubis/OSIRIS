#!/usr/bin/env python3
"""
OSIRIS uartboot: boot a TI DA830 / OMAP-L137 over UART without the TI Windows tools.

Stage 1 (boot ROM, AIS over UART, as TI AISParse.cs):
    start-word sync -> ping opcode sync -> every AIS command opcode-synced, then its arguments.
Stage 2 (optional, TI "sft" serial flash target speaking BOOTUBL/CMD, as sfh.cs):
    BOOTUBL -> "    CMD" + magic -> DONE -> SENDIMG -> "    ACK" + header -> BEGIN -> data -> DONE

The ROM's UART timing assumes a 24 MHz crystal. On a board with a 20 MHz crystal its nominal
115200 baud is really 115200 * 20/24 = 96000 (-b 96000); TI's sft assumes 24 MHz as well, so it
also talks at 96000 there.

Stage 2b (optional, dspboot): after the AIS stage switch to the console baud (115200), wait for
dspboot's "ymodem: ready" + 'C' (or interrupt its autoboot and type "loady"), send FILE with the
built-in YMODEM sender, then "bootram".

Usage:
    uartboot.py -p /dev/cu.usbserial-XXX --ais sft.bin                       # just run an AIS image
    uartboot.py -p /dev/cu.usbserial-XXX --ais sft.bin --app u-boot.bin      # sft + app to RAM
    uartboot.py -p /dev/cu.usbserial-XXX --ais dspboot.ais --ymodem u-boot.bin [--console]

Needs pyserial (pip install pyserial). The port is COM3 etc. on Windows,
/dev/ttyUSB0 on Linux, /dev/cu.usbserial-XXXX on macOS.
"""

import argparse
import os
import struct
import sys
import time

try:
    import serial                       # pyserial
except ImportError:                     # the host tests only use the protocol code
    serial = None

AIS_MAGIC = 0x41504954
XMT_START = 0x58          # MSB of 0x58535441, UART syncs on 8 bits
RCV_START = 0x52          # MSB of 0x52535454

OP_SECTION_LOAD = 0x58535901
OP_REQUEST_CRC = 0x58535902
OP_ENABLE_CRC = 0x58535903
OP_DISABLE_CRC = 0x58535904
OP_JUMP = 0x58535905
OP_JUMP_CLOSE = 0x58535906
OP_SET = 0x58535907
OP_START_OVER = 0x58535908
OP_CMP_SECTION_LOAD = 0x58535909
OP_SECTION_FILL = 0x5853590A
OP_PING = 0x5853590B
OP_FUNCTION_EXEC = 0x5853590D
OP_SEQ_READ_ENABLE = 0x58535963

POS_N = 2
OPCODE_DELAY = 0.005

# sft (serial flash target) commands
SFT_MAGIC_FLASH_NO_UBL = 0x55424C00    # patched sft: receive image, copy to DDR, wake the ARM
SFT_MAGIC_BINARY_BOOT = 0x55424CBB


def log(msg):
    print(msg, flush=True)


class Port:
    """Serial port (pyserial: COMx on Windows, /dev/... on Linux and macOS)."""

    def __init__(self, path, baud):
        if serial is None:
            sys.exit('these tools need pyserial: pip install pyserial')
        self.s = serial.Serial(path, baud, timeout=0)
        self.s.reset_input_buffer()
        self.s.reset_output_buffer()

    def set_baud(self, baud):
        self.s.baudrate = baud

    def write(self, data):
        self.s.write(data)
        self.s.flush()                      # wait until it is on the wire

    def read(self, n, timeout):
        out = b''
        end = time.monotonic() + timeout
        while len(out) < n:
            left = end - time.monotonic()
            if left <= 0:
                break
            self.s.timeout = left
            out += self.s.read(n - len(out))
        return out

    def flush_input(self):
        self.s.reset_input_buffer()

    def close(self):
        self.s.close()


def wait_for_port(path):
    """Wait until the serial adapter is back (it can drop out while the board power-cycles)."""
    while True:
        try:
            serial.Serial(path).close()
            break
        except (OSError, serial.SerialException):
            time.sleep(0.2)
    time.sleep(0.5)


def u32(v):
    return struct.pack('<I', v & 0xFFFFFFFF)


def ack_of(opcode):
    return (opcode & 0xF0FFFFFF) | 0x02000000


class AisBoot:
    def __init__(self, port, timeout=5.0):
        self.port = port
        self.timeout = timeout

    def word(self, v):
        self.port.write(u32(v))

    def read_word(self):
        b = self.port.read(4, self.timeout)
        if len(b) != 4:
            raise IOError('timeout waiting for a word (got %r)' % b)
        return struct.unpack('<I', b)[0]

    def sws(self):
        log('ROM: start-word sync...')
        while True:
            self.port.write(bytes([XMT_START]))
            b = self.port.read(1, 0.2)
            if b and b[0] == RCV_START:
                return

    def pos(self):
        self.word(OP_PING)
        if self.read_word() != ack_of(OP_PING):
            return False
        time.sleep(OPCODE_DELAY)
        self.word(POS_N)
        if self.read_word() != POS_N:
            return False
        for i in range(1, POS_N + 1):
            time.sleep(OPCODE_DELAY)
            self.word(i)
            if self.read_word() != i:
                return False
        return True

    def opcode_sync(self, op):
        for _ in range(10):
            self.word(op)
            try:
                if self.read_word() == ack_of(op):
                    return
            except IOError:
                self.word(0)            # clear a possibly half-received word on the target
        raise IOError('opcode sync failed for 0x%08x' % op)

    def boot(self, ais):
        cur = 0

        def take():
            nonlocal cur
            v = struct.unpack_from('<I', ais, cur)[0]
            cur += 4
            return v

        if take() != AIS_MAGIC:
            raise ValueError('not an AIS image')
        while True:
            self.sws()
            log('ROM: ping sync...')
            if self.pos():
                break
        while True:
            time.sleep(OPCODE_DELAY)
            op = take()
            self.opcode_sync(op)
            if op in (OP_SECTION_LOAD, OP_CMP_SECTION_LOAD):
                addr, size = take(), take()
                padded = (size + 3) & ~3
                log('ROM: section 0x%08x, %d bytes' % (addr, size))
                self.word(addr)
                self.word(size)
                self.port.write(ais[cur:cur + padded])
                cur += padded
            elif op == OP_SECTION_FILL:
                addr, size, typ, pattern = take(), take(), take(), take()
                log('ROM: fill 0x%08x, %d bytes with 0x%08x' % (addr, size, pattern))
                for v in (addr, size, typ, pattern):
                    self.word(v)
            elif op == OP_SET:
                vals = [take() for _ in range(4)]          # type, addr, data, sleep
                log('ROM: set 0x%08x = 0x%08x' % (vals[1], vals[2]))
                for v in vals:
                    self.word(v)
            elif op == OP_FUNCTION_EXEC:
                fa = take()
                nargs = fa >> 16
                args = [take() for _ in range(nargs)]
                log('ROM: function %d (%s)' % (fa & 0xFFFF, ', '.join('0x%x' % a for a in args)))
                self.word(fa)
                for a in args:
                    self.word(a)
            elif op == OP_REQUEST_CRC:
                got = self.read_word()
                want, seek = take(), take()
                if got != want:
                    raise IOError('CRC mismatch: device 0x%08x, image 0x%08x' % (got, want))
                log('ROM: CRC ok')
            elif op in (OP_ENABLE_CRC, OP_DISABLE_CRC, OP_SEQ_READ_ENABLE):
                pass
            elif op in (OP_JUMP, OP_JUMP_CLOSE):
                addr = take()
                self.word(addr)
                log('ROM: jump to 0x%08x%s' % (addr, ' (close)' if op == OP_JUMP_CLOSE else ''))
                if op == OP_JUMP_CLOSE:
                    return
            else:
                raise ValueError('unsupported AIS opcode 0x%08x at offset %d' % (op, cur - 4))


def wait_for(port, token, timeout=30.0, echo=True):
    """Wait for an sft token like 'BOOTUBL\\0'; prints whatever else the target says."""
    token = token.encode() + b'\0'
    buf = b''
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        b = port.read(1, 0.5)
        if not b:
            continue
        buf = (buf + b)[-256:]
        if echo and (32 <= b[0] < 127 or b in b'\r\n'):
            sys.stdout.write(b.decode())
            sys.stdout.flush()
        if buf.endswith(token):
            return True
    return False


def sft_send_app(port, app, load_addr, entry):
    log('\nSFT: sending START, waiting for BOOTUBL...')
    for _ in range(30):                     # the sft waits for "  START" before it answers
        port.write(b'  START\0')
        if wait_for(port, 'BOOTUBL', timeout=1.0):
            break
    else:
        raise IOError('no BOOTUBL from sft')
    port.write(b'    CMD\0' + b'%08X' % SFT_MAGIC_FLASH_NO_UBL)
    if not wait_for(port, '   DONE'):
        raise IOError('sft did not accept the command')
    if not wait_for(port, 'SENDIMG'):
        raise IOError('no SENDIMG from sft')
    # magic, entry point, byte count, load address, flash address (unused by the RAM loader), "0000"
    hdr = b'    ACK\0' + b'%08X%08X%08X%08X%08X' % (SFT_MAGIC_BINARY_BOOT, entry, len(app), load_addr, 0) + b'0000'
    port.write(hdr)
    if not wait_for(port, '  BEGIN'):
        raise IOError('no BEGIN from sft')
    log('\nSFT: sending %d bytes...' % len(app))
    for i in range(0, len(app), 4096):
        port.write(app[i:i + 4096])
        sys.stdout.write('\r  %3d%%' % ((i + 4096) * 100 // len(app) if i + 4096 < len(app) else 100))
        sys.stdout.flush()
    log('')
    if not wait_for(port, '   DONE', timeout=60):
        raise IOError('sft did not confirm the image')
    log('\nSFT: image accepted; the ARM should be starting it now')


# YMODEM sender (CRC-16, 1024-byte blocks, 128-byte blocks for short tails)

YM_SOH, YM_STX, YM_EOT, YM_ACK, YM_NAK, YM_CAN, YM_C = 0x01, 0x02, 0x04, 0x06, 0x15, 0x18, 0x43


def crc16_xmodem(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def ym_block(num, payload, size):
    payload = payload.ljust(size, b'\x1a' if num else b'\0')
    crc = crc16_xmodem(payload)
    return bytes([YM_STX if size == 1024 else YM_SOH, num & 0xFF, 0xFF - (num & 0xFF)]) + payload + \
        bytes([crc >> 8, crc & 0xFF])


def ym_wait(port, wanted, timeout):
    """Wait for one of the bytes in 'wanted'; returns it or None. Two CANs abort."""
    end = time.monotonic() + timeout
    cans = 0
    while time.monotonic() < end:
        b = port.read(1, min(0.5, max(0.01, end - time.monotonic())))
        if not b:
            continue
        if b[0] == YM_CAN:
            cans += 1
            if cans >= 2:
                raise IOError('YMODEM: receiver cancelled')
            continue
        if b[0] in wanted:
            return b[0]
    return None


def ym_send_block(port, blk, retries=10, timeout=5.0):
    for _ in range(retries):
        port.write(blk)
        r = ym_wait(port, (YM_ACK, YM_NAK), timeout)
        if r == YM_ACK:
            return
    raise IOError('YMODEM: block %d not acknowledged' % blk[1])


def ymodem_send(port, data, name='image.bin', progress=True, wait_c=60.0):
    """Send one file. Assumes the receiver is (about to start) sending 'C'."""
    if ym_wait(port, (YM_C,), wait_c) is None:
        raise IOError("YMODEM: no 'C' from receiver")
    hdr = name.encode()[:100] + b'\0' + str(len(data)).encode() + b' 0 0'
    ym_send_block(port, ym_block(0, hdr, 128))
    if ym_wait(port, (YM_C,), 10) is None:
        raise IOError("YMODEM: no 'C' after header block")
    num, ofs, t0 = 1, 0, time.monotonic()
    while ofs < len(data):
        size = 1024 if len(data) - ofs > 128 else 128
        ym_send_block(port, ym_block(num, data[ofs:ofs + size], size))
        ofs += size
        num += 1
        if progress:
            sys.stdout.write('\r  YMODEM %d/%d bytes (%d%%)' % (min(ofs, len(data)), len(data),
                                                              min(ofs, len(data)) * 100 // max(1, len(data))))
            sys.stdout.flush()
    for _ in range(10):                      # EOT; receiver NAKs the first one, ACKs the repeat
        port.write(bytes([YM_EOT]))
        r = ym_wait(port, (YM_ACK, YM_NAK), 5)
        if r == YM_ACK:
            break
    else:
        raise IOError('YMODEM: EOT not acknowledged')
    if ym_wait(port, (YM_C,), 5) is not None:  # end of batch: empty block 0
        ym_send_block(port, ym_block(0, b'', 128))
    if progress:
        dt = time.monotonic() - t0
        log('\n  YMODEM done: %d bytes in %.1f s (%.0f B/s)' % (len(data), dt, len(data) / max(dt, 1e-3)))


def dspboot_ymodem(port, data, name, boot=True, timeout=60.0):
    """Drive dspboot's console: interrupt autoboot, 'loady', send, 'bootram'."""
    buf = b''
    sent_key = sent_loady = False
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        b = port.read(1, 0.5)
        if not b:
            continue
        if 32 <= b[0] < 127 or b in b'\r\n':
            sys.stdout.write(b.decode())
            sys.stdout.flush()
        buf = (buf + b)[-512:]
        if not sent_key and (b'Hit any key' in buf or b'Press any key' in buf):
            port.write(b' ')
            sent_key = True
        elif not sent_loady and buf.endswith(b'dspboot> '):
            port.write(b'loady\r')
            sent_loady = True
        elif b'ymodem: ready' in buf and buf.endswith(b'\n'):
            break                            # dspboot sends 'C' next (fallback or loady)
    else:
        raise IOError('no YMODEM prompt from dspboot')
    log('')
    ymodem_send(port, data, name)
    if not wait_for_text(port, b'ymodem: received', 10):
        raise IOError('dspboot did not confirm the upload')
    if sent_loady and boot:
        if not wait_for_text(port, b'dspboot> ', 10):
            raise IOError('no prompt after upload')
        port.write(b'bootram\r')
    elif not sent_loady:
        log('\n(dspboot fallback path: it validates and starts the image by itself)')


def wait_for_text(port, token, timeout):
    buf = b''
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        b = port.read(1, 0.5)
        if not b:
            continue
        if 32 <= b[0] < 127 or b in b'\r\n':
            sys.stdout.write(b.decode())
            sys.stdout.flush()
        buf = (buf + b)[-256:]
        if buf.endswith(token) or token in buf:
            return True
    return False


def main():
    ap = argparse.ArgumentParser(description='OSIRIS UART boot for DA830 / OMAP-L137')
    ap.add_argument('-p', '--port', required=True)
    ap.add_argument('-b', '--baud', type=int, default=96000,
                    help='ROM baud (default 96000 = 115200 with a 20 MHz crystal)')
    ap.add_argument('--ais', required=True, help='AIS image for the boot ROM (e.g. the sft)')
    ap.add_argument('--app', help='binary to send through the sft (e.g. u-boot.bin)')
    ap.add_argument('--ymodem', metavar='FILE',
                    help='after the AIS stage, send FILE to dspboot with YMODEM and boot it')
    ap.add_argument('--no-boot', action='store_true', help='with --ymodem: only upload, do not bootram')
    ap.add_argument('--load', type=lambda x: int(x, 0), default=0xC1080000)
    ap.add_argument('--entry', type=lambda x: int(x, 0), default=0xC1080000)
    ap.add_argument('--console', action='store_true', help='stay attached and print the console')
    ap.add_argument('--console-baud', type=int, default=115200)
    args = ap.parse_args()

    ais = open(args.ais, 'rb').read()
    log('power on / reset the board in UART boot mode now')
    while True:                              # the adapter may vanish while the board power-cycles
        try:
            port = Port(args.port, args.baud)
            AisBoot(port).boot(ais)
            break
        except OSError as e:
            log('serial port: %s - waiting for it to come back' % e)
            try:
                port.close()
            except Exception:
                pass
            wait_for_port(args.port)
    if args.app:
        sft_send_app(port, open(args.app, 'rb').read(), args.load, args.entry)
    if args.ymodem:
        port.set_baud(args.console_baud)
        port.flush_input()
        log('--- dspboot console at %d ---' % args.console_baud)
        dspboot_ymodem(port, open(args.ymodem, 'rb').read(), os.path.basename(args.ymodem),
                       boot=not args.no_boot)
    if args.console:
        port.set_baud(args.console_baud)
        log('--- console at %d (ctrl-c to quit) ---' % args.console_baud)
        try:
            while True:
                b = port.read(256, 0.2)
                if b:
                    sys.stdout.write(b.decode(errors='replace'))
                    sys.stdout.flush()
        except KeyboardInterrupt:
            pass


if __name__ == '__main__':
    main()
