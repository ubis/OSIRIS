# Installing OSIRIS

## What you need

- A Linux machine with Docker and git to build the firmware. The build makes
  its own toolchains; nothing else needs to be installed.
- A USB serial adapter with **3.3 V** signals, connected to the console pins
  of the receiver's DSP board (UART1), and Python 3 with pyserial on the
  computer it is plugged into.
- A way to switch the chip between flash (NAND) and serial (UART) boot. On
  the Onkyo boards that means soldering a DIP switch to its BOOT pins
  ([boot-switch.md](boot-switch.md)).
- An Ethernet cable and a TFTP server on the same network, for loading the
  installer.

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

| file | what |
|---|---|
| `dist/dspboot/<board>/` | dspboot: `dspboot.ais` for the serial cable, `dspboot-512b.nand` (`-2k`, `-4k`) for the flash |
| `dist/images/osiris-<version>.tar` | the bundle: kernel, root file system and U-Boot, for installing and updating |
| `dist/images/uImage-installer` | the installer: Linux with the system and the bundle built in, runs from RAM |

Without building, take the same files from the
[releases](https://github.com/ubis/osiris/releases): `dspboot-<version>.zip`,
`osiris-<version>.tar` and `uImage-installer`.

## Trying it from RAM

dspboot, U-Boot and the installer can all run from RAM, and the installer
runs the whole system with its settings in RAM. Nothing is written to the
flash; the next power-on starts whatever is on it. You need the same things
as for an install, except the backup.

1. **dspboot and U-Boot.** Take U-Boot out of the bundle:

   ```sh
   tar -xf osiris-<version>.tar u-boot.dbi
   ```

   On a board that already has dspboot in the flash, open the console at
   115200 baud in a terminal that can send YMODEM (picocom with `sz`,
   minicom, or Tera Term on Windows), power it on and press a key at
   `Hit any key to stop autoboot`. At the `dspboot>` prompt type `loady`,
   send `u-boot.dbi` and type `bootram`.

   On a board with the stock firmware, put it into UART boot mode, power it
   on and let uartboot start dspboot, send U-Boot and start it:

   ```sh
   python3 dspboot/tools/uartboot.py -p /dev/ttyUSB0 \
       --ais dist/dspboot/onkyo-tx-nr/dspboot.ais --ymodem u-boot.dbi
   ```

   Either way, press a key in the console to stop U-Boot's autoboot.

2. **The installer.** Put `uImage-installer` on your TFTP server and boot
   it as in step 3 of the first install below (`setenv ipaddr`, `setenv
   serverip`, `tftp`, `setenv bootargs`, `bootm`), without `saveenv`. Log
   in over the network or on the console as `root` (password `osiris`), and
   do not run `osiris-install`.

## First install

The first install puts dspboot into the flash over the serial cable, starts
U-Boot from the bundle once, and boots the installer over the network, which
writes the rest. It erases the stock firmware (except your backup, which
you made).

1. **dspboot.** Put the board into UART boot mode, power it on and write
   dspboot into the flash (for chips with 512-byte pages; for others take
   the `-2k`/`-4k` file and offset from [dspboot.md](dspboot.md)):

   ```sh
   python3 dspboot/tools/nandrestore.py -p /dev/ttyUSB0 \
       --ais dist/dspboot/onkyo-tx-nr/dspboot.ais \
       dist/dspboot/onkyo-tx-nr/dspboot-512b.nand@0x4000
   ```

   nandrestore leaves dspboot running at its `dspboot>` prompt.

2. **U-Boot from the bundle.** Take U-Boot out of the bundle:

   ```sh
   tar -xf osiris-<version>.tar u-boot.dbi
   ```

   Open the console at 115200 baud with a terminal that can send YMODEM
   (picocom with `sz`, minicom, or Tera Term on Windows), type
   `loady` at the `dspboot>` prompt, send `u-boot.dbi`, then type
   `bootram`. U-Boot starts from RAM, nothing is written yet; press a key to
   stop its autoboot.

3. **The installer.** Put `uImage-installer` on your TFTP server. In U-Boot,
   with your own addresses (board, TFTP server, gateway, netmask):

   ```
   setenv ipaddr 192.168.1.50
   setenv serverip 192.168.1.10
   tftp 0xc2000000 uImage-installer
   setenv bootargs console=ttyS1,115200n8 mem=56M ip=192.168.1.50:192.168.1.10:192.168.1.1:255.255.255.0::eth0:off
   bootm 0xc2000000
   ```

   The board keeps the MAC address the stock firmware stored (`ethaddr`).
   On a new, empty flash set it first with `setenv ethaddr <address>`, from
   the label on the receiver.

4. **Install.** Log in as `root` (password `osiris`) and run
   `osiris-install`. It writes the bundle built into the installer: U-Boot
   into both slots, the kernel and root file system, and a fresh U-Boot
   environment. To install another bundle, put it on the TFTP server and
   give its name: `osiris-install osiris-<version>.tar`.

   Then switch the board back to NAND boot and power it off and on.

OSIRIS now starts from the flash. The receiver shows "OSIRIS" and the build
as its DSP version, and the board gets its address by DHCP, with the
receiver model as host name (for example `TX-NR717`).

## Updating

From a USB stick: copy `osiris-<version>.tar` to the top directory of a FAT
stick and plug it into the receiver. When the receiver checks for updates
(at power-on, and again every few minutes until it gets an answer) it shows
its update notice, if its update notice setting is on. Confirm, wait for
"Completed!", then switch the receiver off and on with its power button;
Zone 2 and Zone 3 must be off too, or the board stays on and keeps running
the old version until it is restarted. A bundle of the version already
running is not offered.

Over the network, copy the bundle to the receiver and install it:

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
