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
| 66 | item | answer to E6. Item 1: [1] 1, [3] DHCP, [4..23] IP, netmask, gateway, DNS 1, DNS 2 (each a 32-bit little endian value), [24..29] MAC. Item 5: 05 + friendly name |
| 6A | 1 byte | reply to EA, echoes its byte; after 6A 01 the MCU cuts the board's power |
| 73 | 01 "!1..." | ISCP towards the MCU (tried for NTI, no visible effect yet) |

### 0x42 stream status

| byte | value (stock capture: PCM 48 kHz 2.0) | meaning |
|---|---|---|
| 0 | 02 | stream type (0 unlocked, 1 analog, 2 PCM, ...) |
| 1-3 | 09 09 09 | sample rate codes (9 = 48 kHz) |
| 4-5 | 05 00 | unknown |
| 6-7 | 00 00 | flags |
| 8-11 | " 2.0" | channel string for the front panel |
| 12-13 | FF 07 | output channel mask: the MCU closes the speaker relays only for channels set here |
| 14 | 01 | bit 0: DSP detected OK |
| 15-16 | 20 20 | channel layout codes (2/0, guessed) |
| 17-18 | 00 00 | unknown |
| 19 | 01 | always 1 |

Sending 0x42 for every C1 makes the MCU restart its input sequence: AudioApp
only sends it when a field changes.

## MCU -> DSP

| cmd | name (AudioApp) | known payload |
|---|---|---|
| C0 | initial_settings | |
| C1 | input_settings | [1] decode mode [2] interface [3] fs code [4] MCLK [5] status |
| C2 | speaker_config | speaker setup changed (stock answers with 45) |
| C3 | distance | speaker distances |
| C4 | equalizer | |
| C5 | thx | |
| C6 | multiplex_mono | |
| C7 | dd_dts_adsy_td | |
| C8 | dsd_dac_direct | |
| C9 | lfe_level | |
| CA | av_sync | |
| CB | hdmi_audio_tv_out | |
| CC | bass_treble | |
| CD | re_eq | |
| CE | music_optimizer | |
| CF | late_night | |
| D0 | testtone_ch | calibration: channel for the test tone (00, 02, 00, 07 seen) |
| D1 | testmode | |
| D2 | listening_mode | [1] mode, [2] variant, see below; F1 = test tone (calibration) |
| D3 | volume | [1..2] s16 LE, 0.01 dB; for information, the MCU sets it in the analog stage |
| D4 | headphone | |
| D5 | ch_layout | |
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
| E8 | update | firmware update (unused) |
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
variant within it. The MCU also reports the mode as ISCP text (`!1LMDxx`).
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

## Calibration (unfinished)

During automatic level setup the MCU sends D2 F1 (test tone listening mode),
then D0 with the channel and D3 0 dB, turns the analog volume up to 0 dB and
expects the DSP to play test noise on that channel. If the DSP keeps passing audio instead, the MCU keeps the
DAC muted afterwards. AudioApp names: `HostRsv_Testmode_On/Off`,
`uw_dsp_test_tone_set_cmd`, `uw_dsp_test_tone_noise_set_cmd[%d]`. The replies
it expects are not known yet.

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
| `status` | | link state, handshake stage, model |
| `identity` | `dsp_version`, `dsp_id` | strings for 41 and 46 |
| `stream` | 0x42 fields by name | updates the stream status, sent if changed |
| `outputs` | `mask` | 4A |
| `levels` | 13 values | 45 |
| `send` | `cmd`, `data` (hex) | any frame, for experiments |

Events (`ubus listen mcu`):

| event | from | data |
|---|---|---|
| `mcu.ready` | end of start-up | `model`, `make`, `code` |
| `mcu.input` | C1, E5 | decoded fields |
| `mcu.volume` | D3 | `volume` (0.01 dB) |
| `mcu.speakers` | C2, C3, C9 | raw payload until decoded |
| `mcu.listening_mode` | D2 | `mode` (also kept in `mcu status` as `listening_mode`) |
| `mcu.testtone` | D0, D1 | `channel` |
| `mcu.iscp` | F3 | `command` |
| `mcu.frame` | everything else | `cmd`, `name`, `data` (hex) |
