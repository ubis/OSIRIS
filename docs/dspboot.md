# dspboot

dspboot is a small open-source program that starts devices built around the
Texas Instruments **OMAP-L137 / D830** chip, such as the Onkyo TX-NR515 and
TX-NR717 AV receivers and their Integra twins. It does the job of the
manufacturer's first boot stage, and it also works as a repair tool that only
needs a serial cable and a computer.

## What you can do with it

- **Bring a "bricked" board back.** dspboot runs straight from the serial
  cable, without touching what is on the board. No vendor software, no
  JTAG.
- **Restore the stock firmware.** Write a backup of the original flash
  memory (NAND) back exactly as it was, byte for byte, and check it.
- **Replace the NAND chip.** If the flash chip is worn out or dead, solder in
  a new one and write the firmware onto it from the serial cable. dspboot
  also works with today's larger chips (2 KiB and 4 KiB pages), not only the
  old 512-byte-page parts the receivers came with. Note that the stock Onkyo
  firmware itself expects the old page size.
- **Check the hardware.** dspboot reports the chip, the clocks, the memory
  and the flash, tests the memory and lists bad flash blocks.
- **Boot your own firmware.** dspboot starts U-Boot from one of two copies in
  the flash and falls back to the other one if the first is damaged. This is
  how the OSIRIS firmware starts.

## What you need

- A computer with Windows, Linux or macOS, Python 3 and pyserial
  (`pip install pyserial`).
- A USB serial adapter with **3.3 V** signals, connected to the board's
  serial console pins (for the Onkyo receivers: UART1, as used by the stock
  firmware).
- A way to switch the chip between **NAND and UART boot** (its BOOT pins).
  On the Onkyo receivers that means soldering DIP switches to those pins
  ([boot-switch.md](boot-switch.md)).
- The dspboot images: `make dspboot` builds them into `dist/dspboot/`,
  one directory per board. The tools are in `dspboot/tools/`.

| file | what |
|---|---|
| `dist/dspboot/<board>/dspboot.ais` | dspboot itself, for the serial cable or the flash |
| `dspboot/tools/uartboot.py` | starts dspboot over the serial cable |
| `dspboot/tools/nandrestore.py` | writes files into the flash through dspboot and checks them |
| `dspboot/tools/aisdump.py` | reads the settings out of a stock first stage (for new boards) |

## Using it

All commands run from the top of the repository; replace `/dev/ttyUSB0`
with your adapter (`COM3` or similar on Windows, `/dev/cu.usbserial-XXXX` on
macOS) and `<board>` with your board, e.g. `onkyo-tx-nr`.

### Start dspboot over the serial cable

Put the board into UART boot mode, power it on and run:

```sh
python3 dspboot/tools/uartboot.py -p /dev/ttyUSB0 \
    --ais dist/dspboot/<board>/dspboot.ais --console
```

Within a few seconds dspboot reports what it found:

```
dspboot v1.0 (Oct 11 2026 - 10:13:58)

SoC:    OMAP-L137/DA830 rev 2.0
Board:  onkyo-tx-nr
Boot:   UART1
Clocks: DSP/ARM 400 MHz, SDRAM 133 MHz, EMIFA 89 MHz (20 MHz crystal)
DRAM:   64 MiB, quick test OK
NAND:   64 MiB Numonyx, 512 B pages, 16 KiB blocks, 1 bad block
Hit any key to stop autoboot:  1
```

Press a key for the `dspboot>` prompt. `help` lists the commands. Nothing on
the board changes by starting dspboot this way.

The boot ROM's speed depends on the board's crystal: `-b 96000` (the default)
for 20 MHz boards like the Onkyo receivers, `-b 115200` for 24 MHz ones.
dspboot itself always talks at 115200.

### Restore the stock firmware

A backup of the flash consists of files with the data and the spare bytes of
every page (`nanddump --oob` output, for example), one per part of the flash.
Write them back to where they came from:

```sh
python3 dspboot/tools/nandrestore.py -p /dev/ttyUSB0 --ais dist/dspboot/<board>/dspboot.ais \
    mtd1.oob@0x4000 mtd2.oob@0x14000 mtd3.oob@0x54000
```

nandrestore starts dspboot over the cable, then for each file erases the
blocks, writes every page, reads it back and finally compares a checksum of
the whole range with the file. Blocks marked bad are never written. For
2 KiB or 4 KiB page chips add `--page 2048` or `--page 4096`.

### Put dspboot into the flash

To start dspboot from the flash at every power-on (needed for the OSIRIS
firmware), write it to block 1. nandrestore adds the spare bytes with the
boot ROM's ECC for your chip's page size (dspboot shows it on the `NAND:`
line; `--page 2048` or `--page 4096` for the larger ones):

```sh
python3 dspboot/tools/nandrestore.py -p /dev/ttyUSB0 --ais dist/dspboot/<board>/dspboot.ais \
    dist/dspboot/<board>/dspboot.ais@0x4000
```

Block 1 starts one block in: at 0x4000 with 16 KiB blocks, 0x20000 with
128 KiB and 0x40000 with 256 KiB; dspboot shows the block size on the
`NAND:` line. If block 1 is bad, dspboot goes into the next good block,
where the boot ROM looks next, but never past 1 MiB. U-Boot goes into its
slots the same way, from `u-boot.dbi`, each within its 512 KiB.

If dspboot already runs from the flash, stop it at `Hit any key`, close the
terminal and use `-b 115200 --skip-ais` instead of booting it over the cable.

### Commands

| command | |
|---|---|
| `info` | everything about the hardware: chip IDs, boot pins, clocks, memory and flash details, flash layout |
| `memtest` | full memory test (takes a minute) |
| `nand info` | flash details, the bad blocks and where the bad block table is |
| `nand dump <page>` | show one flash page with its spare bytes and ECC state |
| `boot [a\|b]` | start U-Boot from the flash |
| `loady`, `bootram` | receive a program over the cable (YMODEM) and start it |
| `md <addr> [len]` | show memory |
| `reset` | restart the board |

`nand read`, `nand writeraw` and `nand crcraw` are what nandrestore uses.

## Which NAND chips work

For a replacement chip, look for:

- **8-bit** bus (not 16-bit), **SLC** flash, at the voltage the board
  gives the old chip (3.3 V on the Onkyo boards),
- pages of **512 bytes, 2 KiB or 4 KiB** with **16 spare bytes per 512
  bytes** (64 per 2 KiB page, 128 per 4 KiB page),
- **1-bit or 4-bit ECC** required by the data sheet, blocks of at most 256 KiB,
- for OSIRIS, at least **64 MiB** and blocks of at least **16 KiB**,
- either **ONFI** (the data sheet says so; the boot ROM and dspboot then read
  the chip's parameters from it) or one of the device IDs in the boot ROM's
  table (TI's SPRAB04G, appendix B).

Chips that need stronger ECC (most newer small-geometry and all MLC flash)
do not work: the boot ROM and the chip's ECC engine stop at 4 bits per 512
bytes. dspboot reads the chip type from the flash itself and says on the
`NAND:` line if it cannot use it.

## For developers

### Building

```sh
make dspboot                  # all boards, images in dist/dspboot/
make dspboot BOARD=<name>     # one board
make dspboot-test             # unit tests on the build machine
```

The build runs in a Docker image with GCC for the C6000 DSP (`tic6x-elf`),
built from source by `dspboot/docker/Dockerfile`.

### Adding a board

A board is one file in `dspboot/boards/`: crystal frequency, PLL settings, console
UART, SDRAM controller values and NAND timing. `dspboot/boards/template.h` explains
each value, and the build refuses values outside the chip's limits. The right
values are the ones the board's stock first stage gives the boot ROM;
`dspboot/tools/aisdump.py` prints them from a dump of NAND block 1.

| board | crystal | DSP/ARM | SDRAM | EMIFA (NAND) | console |
|---|---|---|---|---|---|
| `onkyo-tx-nr` | 20 MHz | 400 MHz | 133 MHz | 89 MHz | UART1 |
| `onkyo-tx-nr-100` | 20 MHz | 400 MHz | 100 MHz | 89 MHz | UART1 |

### Boot flow and flash layout

dspboot reads `bootslot=a|b` from the U-Boot environment, starts U-Boot from
that slot and falls back to the other one, then waits for an image over
YMODEM. A slot holds `u-boot.bin` wrapped by `dspboot/tools/mkimage.py` (a 64-byte
header with load address, entry point and CRCs). Before starting the ARM,
dspboot leaves a record at 0x8001FF00 saying which slot it started and
whether that was a fallback.

| offset | size | contents |
|---|---|---|
| block 0 | 16 KiB | U-Boot environment |
| block 1 .. 0x0FFFFF | | dspboot |
| 0x100000 | 256 KiB | U-Boot environment, second copy |
| 0x140000 | 512 KiB | U-Boot slot a |
| 0x1C0000 | 512 KiB | U-Boot slot b |
| 0x240000 | rest | the operating system |

The layout is the same for every page size and can be changed per board; it
has to match the board's U-Boot.

## License

BSD-2-Clause, see `dspboot/LICENSE`. The PLL0, PSC and ARM start sequences follow
TI's FlashAndBootUtils 2.40 (BSD-3-Clause, (C) 2012 Texas Instruments),
credited in the sources.
