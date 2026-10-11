# Backups, recovery and the stock firmware

[dspboot](dspboot.md) is the repair tool for these boards. It runs
straight from a serial cable, so it works however broken the flash is, and
it writes the flash with exactly the ECC layout the boot ROM expects. The
same steps work with the stock firmware and with OSIRIS.

You need the serial console, the BOOT pin switch and pyserial, as described
in [install.md](install.md#what-you-need), and the dspboot images from
`make dspboot`.

## Make a backup

Do this first, while the receiver still has its stock firmware. Dump every
flash partition **raw**: with the spare (OOB) bytes and without ECC
correction, for example with mtd-utils from any Linux running on the board:

```sh
nanddump --noecc --oob -f mtd1.oob /dev/mtd1
```

Raw matters: the stock Linux reads the boot partitions with a different ECC
layout from the boot ROM, so a "corrected" dump of them is wrong. Keep the
files, and note at which offset each partition starts (`/proc/mtd` lists
the sizes, in order from 0).

## Start dspboot over the cable

This changes nothing on the board, and works even when nothing on the flash
does. Put the board into UART boot mode, power it on and run:

```sh
python3 dspboot/tools/uartboot.py -p /dev/ttyUSB0 \
    --ais dist/dspboot/onkyo-tx-nr/dspboot.ais --console
```

dspboot reports the chip, the clocks, the memory (with a quick test) and the
flash with its bad blocks. That alone tells you a lot about a dead receiver:
if the chip, SDRAM and NAND show up and test fine, the hardware is probably
good and the firmware is the problem.

## Restore the stock firmware

Write the backup files back to where they came from:

```sh
python3 dspboot/tools/nandrestore.py -p /dev/ttyUSB0 \
    --ais dist/dspboot/onkyo-tx-nr/dspboot.ais \
    mtd1.oob@0x4000 mtd2.oob@0x14000 mtd3.oob@0x54000
```

For every file nandrestore erases the blocks, writes each page with its spare
bytes, reads it back and compares a checksum of the whole range. Blocks
marked bad are skipped. Afterwards switch the board back to NAND boot.

This also works from a backup of another receiver of the same model, but
the U-Boot environment (the first partition) holds the board's MAC address,
so keep your own if you have it.

## Replace the NAND chip

If the flash is worn out, any 8-bit SLC NAND with 512 B, 2 KiB or 4 KiB
pages and at most 4-bit ECC works with dspboot; the
[dspboot README](dspboot.md#which-nand-chips-work) lists what to
look for. Be careful with chips sold as 512-byte-page parts: many are
counterfeit and do not answer the D830 correctly even when a programmer
reads them fine.

- On a new **512-byte-page** chip you can restore the stock firmware as
  above.
- On a **2 KiB or 4 KiB page** chip only OSIRIS runs; the stock firmware
  expects 512-byte pages. Put dspboot and U-Boot on it and follow the
  [first install](install.md#first-install). On a blank chip there is no MAC
  address yet; set `ethaddr` in U-Boot from the label on the receiver.

## Replace the D830

The OMAP-L137 replaces the D830 directly (same package and pins). dspboot
and OSIRIS run on both. The stock firmware boots on the OMAP-L137 but
Onkyo's DSP program does not, so a receiver with a replaced chip needs
OSIRIS for sound. See [no-sound.md](no-sound.md) for why these chips fail.

## Back from OSIRIS to stock

Restore your backup as above. OSIRIS writes its own environment, dspboot and
U-Boot, so restore every partition, not only the large ones.
