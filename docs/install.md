# Installing OSIRIS

## What you need

- A Linux machine with Docker and git to build the firmware. The build makes
  its own toolchains; nothing else needs to be installed.
- A USB serial adapter with **3.3 V** signals, connected to the console pins
  of the receiver's DSP board (UART1), and Python 3 with pyserial on the
  computer it is plugged into.
- A way to switch the chip between flash (NAND) and serial (UART) boot. On
  the Onkyo boards that means soldering a DIP switch to its BOOT pins.
- An Ethernet cable and a TFTP server on the same network for the first
  install.

**Make a backup of the flash before you start.** With the stock firmware
still running, dump every flash partition with its spare bytes
(`nanddump --oob`); dspboot can write these back byte for byte later (see
[recovery.md](recovery.md)).

## Building

```sh
git clone <this repository> osiris
cd osiris
make dspboot      # the first boot stage -> dist/dspboot/
make buildroot    # U-Boot, Linux, the services and the root file system -> dist/images/
```

The first `make buildroot` downloads the sources Buildroot needs (kept in
`buildroot/dl/`) and takes a while; later builds only redo what changed.

| file in `dist/images/` | what |
|---|---|
| `osiris-<version>.tar` | the update bundle: kernel, root file system, U-Boot |
| `u-boot-512b.nand` (`-2k`, `-4k`) | U-Boot for writing into the flash with dspboot |
| `u-boot.dbi` | U-Boot as the slot image dspboot starts |
| `uImage-installer` | Linux with the whole system in RAM, for the first install |
| `uImage`, `rootfs.squashfs` | the kernel and root file system on their own |

## First install

The first install puts dspboot and U-Boot into the flash over the serial
cable, then starts the installer over the network, which writes the rest.
It erases the stock firmware (except your backup, which you made).

1. **dspboot and U-Boot.** Put the board into UART boot mode, power it on
   and write both into the flash (for chips with 512-byte pages; use the
   `-2k`/`-4k` files and offsets from the [dspboot](dspboot.md)
   instructions otherwise):

   ```sh
   python3 dspboot/tools/nandrestore.py -p /dev/ttyUSB0 \
       --ais dist/dspboot/onkyo-tx-nr/dspboot.ais \
       dist/dspboot/onkyo-tx-nr/dspboot-512b.nand@0x4000 \
       dist/images/u-boot-512b.nand@0x140000
   ```

2. **Files for the installer.** Copy these to the root of your TFTP server:

   | from `dist/images/` | as |
   |---|---|
   | `uImage-installer` | `uImage-installer` |
   | `u-boot.dbi` | `osiris-u-boot.dbi` |
   | `uImage` | `osiris-uImage` |
   | `rootfs.squashfs` | `osiris-rootfs.squashfs` |

3. **Start the installer.** Switch the board back to NAND boot, open the
   console at 115200 baud, power on and press a key to stop U-Boot. Then,
   with your own addresses (board, TFTP server, gateway, netmask):

   ```
   setenv ipaddr 192.168.1.50
   setenv serverip 192.168.1.10
   tftp 0xc0700000 uImage-installer
   setenv bootargs console=ttyS1,115200n8 mem=56M ip=192.168.1.50:192.168.1.10:192.168.1.1:255.255.255.0::eth0:off
   bootm 0xc0700000
   ```

   The board keeps the MAC address the stock firmware stored (`ethaddr`).
   On a new, empty flash set it first with `setenv ethaddr <address>`, from
   the label on the receiver.

4. **Install.** Log in as `root` (password `osiris`) and run:

   ```sh
   osiris-install
   reboot
   ```

OSIRIS now starts from the flash. The receiver shows "OSIRIS" and the build
as its DSP version, and the board gets its address by DHCP, with the
receiver model as host name (for example `TX-NR717`).

## Updating

Copy the bundle to the receiver and install it:

```sh
scp dist/images/osiris-<version>.tar root@TX-NR717:/tmp/
ssh root@TX-NR717 osiris-upgrade /tmp/osiris-<version>.tar
```

The update goes into the copy that is not running, and the receiver restarts
into it once. If it comes up, it becomes the default; if it does not, the
next start is the previous version again.

## After installing

Change the root password after the first login (`passwd`); settings, ssh keys
(`ssh-copy-id`) and the host name in `/data/etc/hostname` survive updates.

The services talk over [ubus](https://openwrt.org/docs/techref/ubus), so the
receiver can be watched and controlled from the shell:

```sh
ubus listen                  # everything the MCU reports, as it happens
ubus call mcu status         # model, link state
ubus call dsp status         # DSP state, input levels
```

[services/](services.md) describes each service and its commands.
