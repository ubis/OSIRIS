# MCU link protocol

The receiver's main MCU (Renesas) and the DSP side (on the stock firmware:
`spid` + AudioApp on the D830's ARM) talk over SPI. Everything here was
reverse engineered from the stock TX-NR515/717 firmware and its logs; it is
what `mcud` implements. Unknown parts are marked as such.

## Transport

| signal | pin | |
|---|---|---|
| SPI0 | `/dev/spidev0.0` | mode 0, 8 bit, 1 MHz, full duplex |
| ARM_CS | GPIO 30 (GPIO1_14) | out, active low: chip select, and "I have data" |
| ARM_RDY | GPIO 34 (GPIO2_2) | in, active low: MCU ready for a transfer |

Every transfer is exactly **1032 bytes** in both directions. One cycle, with
the stock `spid` timing:

1. wait for ARM_RDY low; while it is high and frames are queued, hold
   ARM_CS low to ask for a slot (the MCU answers by pulling RDY low)
2. ARM_CS low, 1 ms, transfer 1032 bytes, 0.5 ms, ARM_CS high
3. 2 ms pause; idle polling every 12 ms when nothing is queued

The DSP side sends one queued frame per cycle (or zeros); whatever the MCU
clocks back in the same transfer is its frame for us.

## Frame

```
[cmd] [cnt u16 LE] [len u16 LE] [data: len bytes] [~sum8]
```

- `cnt`: 0xFFFF for a single frame (multi-frame use unknown)
- checksum: inverted 8-bit sum of all bytes from `cmd` to the end of `data`
- `cmd` 0x00 or 0xFF: nothing sent (idle / bus floating)

Commands from the DSP side are 0x40-0x7F, from the MCU 0xC0-0xFF. A reply to
a request uses the request's number minus 0x80 (MCU E0 -> DSP 60, E2 -> 62,
E3 -> 63, EA -> 6A).

## DSP -> MCU

| cmd | payload | meaning |
|---|---|---|
| 40 | none | sent once after 41 at start-up (meaning unknown) |
| 41 | 14 ASCII | DSP version, shown by the receiver (stock "1.02/12609AHA ", ours "OSIRIS <build>") |
| 42 | 20 bytes | stream status, see below; send **only when it changes** |
| 45 | 26 bytes | channel levels: 13 x u16 LE (stock values F0 0A x3, 74 0E x10; encoding unknown) |
| 46 | 16 ASCII | DSP id ("DA83x 1.02  0000") |
| 4A | 1 byte | active outputs; 01 = front pair only (what AudioApp sends) |
| 60 | 13 ASCII | network module version, reply to E0 |
| 62 | none | ask for system info -> E2 |
| 63 | item | ask for system item n (0-7 on this MCU, 0-0x0C on the NR828) -> E3 |
| 64 | 0 / 1 | ask for display info (0 text, 1 OSD) -> E4 |
| 65 | 2 bytes | network module state (00 01 start, 05 00, 05 01, 00 02 ready) |
| 65 | 03 00 port n | USB port state: n 1..31 = storage present, 0 = none; storage makes the MCU check for an update on USB |
| 66 | item | answer to E6. Item 1: [1] 1, [3] DHCP, [4..23] IP, netmask, gateway, DNS 1, DNS 2 (each a 32-bit little endian value), [24..29] MAC. Item 5: 05 + friendly name |
| 68 | type ... | answers in the firmware update, see below |
| 6A | 1 byte | reply to EA, echoes its byte; after 6A 01 the MCU cuts the board's power |
| 73 | 00 or 01, "!1...", CR | ISCP towards the MCU: mcud asks `!1LMDQSTN` (00); 01 with NTI shows nothing |

### 0x42 stream status

| byte | value (stock capture: PCM 48 kHz 2.0) | meaning |
|---|---|---|
| 0 | 02 | stream type (0 unlocked, 1 analog, 2 PCM, ...) |
| 1-3 | 09 09 09 | sample rate codes (9 = 48 kHz) |
| 4-5 | 05 00 | unknown |
| 6-7 | 00 00 | flags |
| 8-11 | " 2.0" | channel string for the front panel |
| 12-13 | FF 07 | output channel mask: the MCU lights the speaker indicators and closes the speaker relays only for channels set here; mcud sets it from the speaker setup |
| 14 | 01 | bit 0: DSP detected OK |
| 15-16 | 20 20 | channel layout codes (2/0, guessed) |
| 17-18 | 00 00 | unknown |
| 19 | 01 | always 1 |

Mask bits on the TX-NR717 (u16; a pair lights from either of its bits):

| bit | indicator |
|---|---|
| 0, 2 | FL FR |
| 1, 13 | C |
| 3, 7 | SL SR |
| 4, 6 | SBL SBR |
| 5 | SB |
| 8 | FL |
| 9, 10 | LH RH |
| 11, 12 | LW RW |
| 14, 15 | SW |

The surround back relay closes for bits 4, 5, 6, 9 and 10. With the FW layout,
LW RW light from bit 11 only while that relay is closed, from bit 12 without it.

Sending 0x42 for every C1 makes the MCU restart its input sequence: AudioApp
only sends it when a field changes. After an input switch (E5), or when the
source comes back on the same input (C1 [5] from 0 to non-zero), the MCU keeps
a digital input such as HDMI muted until a 0x42 arrives; AudioApp's 0x42
changes as the receiver locks, so mcud sends it once then, on the first C1
within a second of E5 or on the return, at most every 3 s.

## MCU -> DSP

| cmd | name (AudioApp) | known payload |
|---|---|---|
| C0 | initial_settings | everything at start-up: [1..10] as C2, [11] and [13..38] as C3 [0] and [1..26], [0xC6] as D5 [1] |
| C1 | input_settings | [1] decode mode [2] interface [3] fs code [4] MCLK [5] status |
| C2 | speaker_config | [1] subwoofers (0, 1, 2), [2..7] front, center, surround, surround back, front high, front wide: 0 none, 1 large, 2..15 small with the crossover 40, 45, 50, 55, 60, 70, 80, 90, 100, 110, 120, 130, 150, 200 Hz; [8] surround back 0 one, 1 two channels; [9] LFE low pass 80, 90, 100, 110, 120 Hz, 5 bypass; [10] double bass; [11..17] wireless; stock answers with 45 |
| C3 | distance | [0] not known (perhaps the unit the menu shows), [1..26] 13 x u16 LE in cm: fl, c, fr, sl, sr, sbl, sbr, fhl, fhr, fwl, fwr, sw, (0); 5 cm steps; sent when changed |
| C4 | equalizer | [2] 00, then 0 manual EQ off, 1 on; [96] 01, then dB per band (signed): 15 bands from 25 Hz to 16 kHz for front, center, surround, surround back, front high, front wide, then 5 from 25 to 160 Hz for the subwoofer. Not in C0: comes when the menu changes it |
| C5 | thx | |
| C6 | multiplex_mono | |
| C7 | dd_dts_adsy_td | [0] which setting: 07 Dynamic EQ (0 off, 1 on); 08 its reference level offset in 5 dB steps, then 60 09 08 07 E8 03 (not known); 0A Dynamic Volume (0 off, then light, medium, heavy); 03 and 06 not known |
| C8 | dsd_dac_direct | |
| C9 | lfe_level | [0] format: DD, DTS, AAC, multichannel PCM, TrueHD, DTS-HD MA, DSD; [1] 0 off, 1 -20 dB, 2 -10 dB, 3 0 dB |
| CA | av_sync | [1..2] u16 LE, ms: the menu's A/V sync less 22 ms (0 up to 20), per input, sent on input changes; AudioApp limits it to 800, 400 or 200 by sample rate |
| CB | hdmi_audio_tv_out | |
| CC | bass_treble | [0] 0 = front, [1] bass, [8] treble: signed dB, -10..+10 in 2 dB steps |
| CD | re_eq | [0] 0 THX Re-EQ, 1 Re-EQ; [1] 0 off, 1 on |
| CE | music_optimizer | [1] 0 off, 1 on |
| CF | late_night | [0] 0 Dolby Digital: [1] 0 off, 1 low, 2 high; [0] 1 TrueHD: [1] 0 on, 1 off, 2 auto |
| D0 | testtone_ch | level calibration: [1] channel 0 fl, 1 c, 2 fr, 3 sr, 4 sbr, 5 sbl, 6 sl, 7 sw, 8 fhl, 9 fhr, 10 fwl, 11 fwr. Comes with listening mode F1; the MCU sets the reference volume (MVL52) and the channel levels itself |
| D1 | testmode | |
| D2 | listening_mode | [1] mode, [2] variant, see below; F1 = test tone (calibration) |
| D3 | volume | [1..2] s16 LE, 0.01 dB, 0 at the reference level; the MCU sets it in the analog stage, OSIRIS reads it for Dynamic EQ |
| D4 | headphone | [1] 1 = headphones plugged in |
| D5 | ch_layout | [1] what the surround back amplifiers drive: 0 nothing, 1 SB, 2 FH, 3 FW, 4 SB+FH, 5 SB+FW, 6 FH+FW, 7 FH+SB(pre), 8 FW+SB(pre), 9 FW+FH(pre), 10 SB(pre), 11 FH(pre), 12 FW(pre), 13 SB+FH+FW(pre) |
| D6 | special_mode | |
| DB | net_mute | |
| DC | netaudio_info | |
| DD | spdifupdate_next | |
| DE | audysseyautosp | |
| DF | | (-> 5F) |
| E0 | | network module version request -> 60 |
| E2 | system info | model code and name as the first two strings ("AVR000E", "TX-NR717") |
| E3 | system item | [0] item number; item 4 = AMX beacon "AMXB<-SDKClass=Receiver><-Make=ONKYO><-Model=...>" |
| E4 | display info | [0] 0 text / 1 OSD |
| E5 | inputsel | input selected |
| E6 | network | [0] item: 0 status, 1 IP settings, 2 proxy, 3 network control, 4 interface, 5 friendly name, 6-0A change a setting, 0B-0D tests. The network menu shows item 1; stock does not answer 2 |
| E8 | update | firmware update, see below |
| EA | power | [0] 00 on, 01 off, 02 factory reset -> 6A with the same byte. 01 comes only when the main zone, Zone 2 and Zone 3 are all off; stock syncs the file systems before answering |
| F1-F3 | update_source / update_mode / update_start, ISCP | F3 01 "!1..." = ISCP from the MCU |

Payload layouts not listed are unknown so far.

## Start-up

1. AudioApp part: **41, 40, 42, 46, 45** in this order.
2. Network module part (from a stock TX-NR828 log, works on the NR515/717 MCU):
   - 65 00 01, then 62 -> E2
   - 63 00 .. 63 07 -> one E3 each
   - 4A 01
   - 64 00 -> E4, 64 01 -> E4
   - 62 -> E2
   - 65 05 00, 65 05 01, 65 00 02: after this the MCU closes the speaker relays
3. The MCU sometimes drops a request: resend after 1 s, give up after 3
   tries and continue with the next step.

## Listening modes (D2)

D2 carries the mode the receiver is in; byte 1 is the mode, byte 2 a
variant within it. The MCU also reports the mode as ISCP text (`!1LMDxx`,
the codes of Onkyo's ISCP documentation) when it changes, and answers
`!1LMDQSTN` (73 00 "!1LMDQSTN" CR); mcud names the mode from that code. It
asks after the handshake and again 5 s later, as the MCU drops the first
question after a cold start.
Seen on the TX-NR717 and checked against the front display:

| D2 [1] | [2] | ISCP | mode |
|---|---|---|---|
| 03 | 00 | LMD01 | Direct |
| 04 | 00 | LMD00 | Stereo |
| 06 | 02 | LMD08 | Orchestra |
| 07 | 02 | LMD09 | Unplugged |
| 08 | 02 | LMD0A | Studio-Mix |
| 0A | 00 | LMD0C | All Ch Stereo |
| 0B | 00 | LMD13 | Full Mono |
| 20 | 02, 03, 04 | LMD81, LMD90, LMD83 | Dolby PLII Music, PLIIz Height, DTS Neo:6 Music (Neo:6 Cinema: 04 03) |
| 50 | 02, 03, 04 | LMD8B, LMD95, LMD8C | THX versions of the above |
| D0 | 01 | LMDA1 | PLII Music + Audyssey DSX |
| F1 | | | test tone (automatic level setup) |

## Firmware update (E8 / 68)

The MCU runs the update screens; the network side only answers. E8 [0]:

| E8 | meaning |
|---|---|
| 00 src | the MCU looks for an update on the network (0) or USB (1) |
| 01 versions | its parts' versions (7 x 5 ASCII: DSP2 VSP1 VMPU OSD DSP1 NET MMPU): is there an update? |
| 03 part | download it |
| 04 | write it |
| 05 | send the MCU its own firmware (68 type 2 blocks, the MCU flashes itself): never answered |

68 [0] is the type, [1] a code (0 success; 0x1A "DL Error", 0x28 "Update Error"):

| 68 | meaning |
|---|---|
| 00 code n0..n6 | answer to E8 01: 1 for each part that has an update |
| 01 code size | download done, size u32 LE; the MCU then sends E8 04 |
| 03 percent | progress; the MCU gives up after ~20 s without one |
| 04 code | written; taken only after progress 100 |

The MCU asks (E8 00 + 01) at power-on and again about every 150 s until it
gets an answer, and when USB storage appears (65 03). An answer with a part
set to 1 to the power-on check shows the update notice (if the receiver's
update notice setting is on) and adds "Update via NET"; to a USB check it
adds "Update via USB" to Setup / Firmware Update.

OSIRIS answers for part 4 (DSP1, shown as "ARM"): the MCU sends E8 03 04,
shows "ARM writing" after E8 04 and "Completed!" after 68 04 00.

Part 5 (NET) does not work here: after the result the MCU checks the
BridgeCo network module of other models, which this board does not have,
and shows "Error 9-1".

Errors are shown as `<part>-<code>`: part 9 NET, 6 ARM, the code as in the
68 answers.

## Level calibration

During the speaker level setup the MCU sends D2 F1 (test tone listening
mode), then D0 with the channel and D3 0 dB, and turns the analog volume up
to its reference level. The DSP plays test noise on that channel; OSIRIS
plays pink noise at -70 dBFS RMS. The MCU applies the levels itself in the
analog stage and sends nothing back for them. The distances come as C3 (and
in C0 at start-up); osirisd passes them on, and the DSP holds the nearer
speakers back.

## mcud

`mcud` owns the link. It answers what the MCU expects at
once, from state it holds, and turns everything else into ubus events.

Answered by mcud itself:
- the start-up sequence and its retries
- E0 -> 60, EA -> 6A
- 41 / 46 from its identity strings (default from `/etc/osiris-version`,
  replaced through `mcu identity`)
- 42 / 45 / 4A when the values set through ubus change

ubus object `mcu`:

| method | arguments | |
|---|---|---|
| `status` | | handshake stage, model, stream status, and once known: bass and treble, the listening mode (`listening_mode`, `listening_mode_name`), the distances, the speaker setup, the input's `interface` (C1 [2]), `update_check` (the MCU waits for an answer to its update check) |
| `identity` | `dsp_version`, `dsp_id` | strings for 41 and 46 |
| `stream` | 0x42 fields by name | updates the stream status, sent if changed; the next speaker setup frame sets the mask again |
| `outputs` | `mask` | 4A |
| `levels` | 13 values | 45 |
| `update` | one of `available` (bool), `progress` (%), `downloaded` (bytes), `result` (`ok`/`error`) | the 68 answers; a failed download is told as a failed 68 01 |
| `usb` | `present`, `port` | 65 03 |
| `send` | `cmd`, `data` (hex) | any frame, for experiments |

Events (`ubus listen mcu`):

| event | from | data |
|---|---|---|
| `mcu.ready` | end of start-up | `model`, `make`, `code` |
| `mcu.input` | C1, E5 | decoded fields |
| `mcu.volume` | D3 | `volume` (0.01 dB); also in `mcu status` |
| `mcu.audyssey` | C7 | `dynamic_eq`, `dynamic_eq_offset` (dB), `dynamic_volume` (0 off to 3), each once the MCU has sent it; also in `mcu status` |
| `mcu.speakers` | C0, C2, D5 | `speakers` (crossover in Hz by speaker, 0 large, missing ones left out), `subwoofers`, `surround_back_channels`, `lfe_lowpass` (0 bypass), `double_bass`, `layout`; also in `mcu status` |
| `mcu.lfe_level` | C9 | `format`, `level` (dB, -100 off) |
| `mcu.av_sync` | CA | `ms`, as the MCU sends it; also in `mcu status` |
| `mcu.equalizer` | C4 | `equalizer`: `eq_on` and a list of band levels in dB for each speaker; also in `mcu status` |
| `mcu.listening_mode` | D2, F3 !1LMD | `mode` (the D2 byte), `name` (from the ISCP code); also in `mcu status` as `listening_mode` and `listening_mode_name` |
| `mcu.testtone` | D0 | `channel` |
| `mcu.distance` | C0, C3 | `distances` (cm by speaker); C3 [0] is only logged |
| `mcu.iscp` | F3 | `command` |
| `mcu.update` | E8 | `stage` (`check`, `download`, `install`, `upload`), `source` (`network`, `usb`) |
| `mcu.frame` | everything else | `cmd`, `name`, `data` (hex) |
