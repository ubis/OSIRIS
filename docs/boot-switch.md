# The boot mode switch

The DA830 reads its BOOT pins at reset and starts from the NAND flash or
from the serial console (UART1). Once dspboot is in the flash, no switch is
needed; with one, the board can always be started over the cable
([recovery.md](recovery.md)), whatever the flash holds.

## TX-NR515 and TX-NR717

NAND and UART1 boot differ in two pins (table 6 of TI's "Using the
OMAP-L1x7 Bootloader", [SPRAB04G](https://www.ti.com/lit/pdf/sprab04)). On
both boards their pull resistors are in the group R3107 to R3120, "for
debugging" on the schematic (on other models, check yours):

| pin | NAND | UART1 | fitted | switch |
|---|---|---|---|---|
| BOOT7 | 0 | 1 | R3108, 10 kΩ to ground | 1 kΩ to 3.3 V at R3107: 3.0 V |
| BOOT2 | 1 | 0 | R3115, 3.3 kΩ to 3.3 V | 1 kΩ to ground at R3116: 0.77 V, low below 0.8 V |

BOOT0, BOOT1 and BOOT3 are high in both modes. A DIP switch takes both
lines: open boots from NAND, closed from UART1. Switch with the receiver
off; the pins are read at reset only.

## Checking it

On UART1, with the console at 96000 baud (the boot ROM's rate with the
20 MHz crystal), the boot ROM sends `BOOTME` twice after power-on.
If nothing comes, check both pins: with one switched the chip picks
another boot mode.
