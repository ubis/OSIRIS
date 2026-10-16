# Features

What works with OSIRIS, tested on the TX-NR717.

## System

| feature | status |
|---|---|
| Boot from NAND (dspboot, U-Boot, Linux) | ✅ |
| Ethernet (DHCP, NTP), ssh | ✅ |
| A/B updates with fallback to the previous version | ✅ |
| Updates from a USB stick through the receiver's update screens | ✅ |
| The receiver accepts the board as its DSP | ✅ |
| Model detection, host name after the model | ✅ |
| Network menu (address, MAC, name) | ✅ |
| Standby: board switched off, data synced | ✅ |
| Volume, front panel, remote | ✅ (the MCU's) |
| Factory reset | ❓ not tested |
| 12 V trigger, IR input | ❓ not tested |
| Network control (Onkyo Remote app) | ❌ not yet |

## Inputs

| feature | status |
|---|---|
| Analog inputs (RCA) | ✅ |
| FM and AM radio, RDS | ✅ |
| HDMI audio, PCM stereo and multichannel, up to 96 kHz | ✅ (7.1 tested at 48 kHz) |
| Dolby Digital | ✅ |
| DTS, Dolby Digital Plus, TrueHD | ❌ muted |
| Optical input, PCM and Dolby Digital | ✅ |
| HDMI ARC from the TV, PCM | ✅ |
| Coaxial input | ❓ not tested |
| Phono input | ❓ not tested |
| On-screen menu over HDMI, HDMI CEC (input and volume from the TV) | ✅ (the MCU's own) |
| HDMI video switching | ❓ not tested |

## Speakers and room

| feature | status |
|---|---|
| Sound from all channels (Stereo, All Ch Stereo, Full Mono) | ✅ |
| Speaker levels | ✅ (the MCU's) |
| Automatic level setup (test noise) | ✅ |
| Speaker distances (delays) | ✅ |
| Bass management (crossovers, double bass), subwoofer pre-out | ✅ |
| Surround back speakers from 5.1 sources | ✅ ¹ |
| Zones, powered speakers (analog sources such as FM) | ✅ |
| Headphones: all channels folded into them, no room EQ | ✅ |
| Zone line outputs, the other 7.2 pre-outs | ❓ not tested |
| Audyssey 2EQ | ❌ not yet |

## Sound modes and processing

| feature | status |
|---|---|
| Bass and treble (front) | ✅ |
| Manual equalizer | ✅ |
| A/V sync | ✅ |
| Dynamic EQ, Dynamic Volume | ✅ ¹ |
| Surround from stereo in the Surround, PLII, Neo:6, Neural and THX Cinema/Music/Games modes | ✅ ¹ |
| Sound field modes (Orchestra, Unplugged, Studio-Mix, TV Logic, ...) | ✅ ¹ |
| Late Night | ✅ for Dolby Digital (how much it does depends on the stream) |
| Re-EQ, Music Optimizer | ❌ not yet |

¹ OSIRIS's own processing: it does what these features are for, but it
is not Dolby's, DTS's, THX's, Audyssey's or Onkyo's algorithm, so it
does not sound exactly like the stock firmware.

## NET input

| feature | status |
|---|---|
| AirPlay from an iPhone, iPad or Mac | ✅ ² |
| Front panel: "AirPlay" and the album, title and artist, a level meter; DISPLAY switches | ✅ ² |
| Spotify Connect, internet radio, DLNA | ❌ not yet |
| USB playback, the NET menu on the TV | ❌ not yet |
| Switching to NET when a phone starts playing | ✅ ² |
| The AirPlay volume slider sets the receiver's volume | ✅ ² up to -40 dB |
| The remote's arrow keys skip tracks on the phone | ❌ not yet |

² New with OSIRIS: not in the stock firmware.
