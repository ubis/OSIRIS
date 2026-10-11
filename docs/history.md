# History

## How it started

About a year ago, after a couple of successful AV receiver repairs (Yamaha HTR-2067, Onkyo TX-NR828), I got a bit braver and bought an Onkyo TX-NR515 and a TX-NR717. At first I found nothing wrong with them - after a reset both started working. Until they stopped. So I had to dig deeper, and that's how I came across the notorious "Onkyo no sound, DTS chip failure" problem.

## Is it really the DTS chip?

Yes, and no. I wasted way too much time being sure the DTS chip was bad, while there was another, quite serious problem in these receivers - the NAND chip.

It's an old NAND with 512 byte pages. You can't get those anywhere anymore, everything you can buy has 2K or 4K pages, and the stock software doesn't run on them that easily.

So I went looking for 512 byte NANDs anyway. I spent countless hours on it: soldered wires to all 8 data lines and all the control lines of the NAND and hooked up a 16 channel logic analyzer, only to find out that some of the chips I bought were counterfeit - they read and write fine in my XGecu programmer, but answer the D830 with nothing but FFs.

Then I thought I'd found working ST NAND chips, and they still wouldn't boot. After reading the datasheets many times I noticed a tiny but crucial detail: the D830 boot ROM handles the NAND OOB (spare bytes) itself and expects its own layout there. That was the last missing piece, and used 512 byte NANDs finally booted - until they died too, later.

At some point I even binary patched Onkyo's kernel so it would boot from a 2K page NAND, because that's what I could actually buy. The kernel booted, but the whole DSP/DTS software still wouldn't run.

## Why the chip dies

I was curious how a chip like that can just die. The first thing I found was "premature wear of buffers" - but what does that actually mean? After digging more, I think I understand it: the chip itself is fine, but the control circuits inside wear out and can't drive the pins to 1s and 0s anymore. So it can't initialize the SDRAM, read the NAND, and so on.

No, a heatsink on the D830 will not help. What may help is lowering its 3.3V supply to 3.1V, though I never tried that.

So there are two problems, and neither chip - the NAND or the DSP - can be bought, unless it's recycled junk.

## OMAP-L137

Then I found a real 1:1 replacement for the D830: the OMAP-L137. Same core, same datasheet, same everything. It even boots the Onkyo firmware (just not Dolby's DSP firmware).

And with it came TI's great datasheets and examples - U-Boot, kernel sources, everything I needed to write my own open source firmware for these receivers. One that runs on both the D830 and the OMAP-L137, and on the newer NANDs with 2K or 4K pages that you can still buy.

That's what led to OSIRIS - built on TI's sources, for receivers like mine.
