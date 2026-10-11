# Services

The programs that make the board work as the receiver's DSP. They run on
the receiver's Linux and talk to each other over
[ubus](https://openwrt.org/docs/techref/ubus):

```
  receiver MCU ── SPI ── mcud ──┐
                                ├── ubus ── osirisd (what to do)
  DSP (dspaudio) ── L2 ── dspd ─┘
```

| service | what it does |
|---|---|
| [mcud](../services/mcud/) | the link to the receiver's main MCU: answers it, reports what it says |
| [dspd](../services/dspd/) | loads and runs the DSP firmware, sets its gains and routing, owns the mute lines |
| [dspaudio](../services/dspaudio/) | the DSP firmware: moves the audio from the inputs to the DAC outputs |
| [osirisd](../services/osirisd/) | the policy: decides what happens when the MCU reports something |

mcud and dspd only do what they are told and report what happens; every
decision is in osirisd, a LuaJIT script that is easy to change on the
receiver itself.

`/etc/init.d/S90osiris` starts them at boot. They log to syslog (`logread`);
started by hand from a terminal, they log there instead.

## mcud

The receiver's MCU runs the front panel, the remote, the inputs and the
amplifiers, and expects a DSP on the other end of an SPI link. mcud is that
other end: it does the start-up handshake, answers what needs an answer at
once (versions, stream status, levels) and turns everything else into
events.

```sh
ubus listen                       # watch the events
ubus call mcu status              # link state, handshake stage, model
mcud -v                           # (with S90osiris stopped) log every frame
```

| event | when |
|---|---|
| `mcu.ready` | handshake done, with the receiver model |
| `mcu.volume` | volume changed, in 0.01 dB (the MCU applies it itself) |
| `mcu.input` | input or stream format changed |
| `mcu.listening_mode` | listening mode changed (0xF1: test tone) |
| `mcu.speakers`, `mcu.testtone` | speaker setup, calibration |
| `mcu.iscp` | ISCP text commands, e.g. `!1MVL21` |
| `mcu.frame` | anything not decoded yet, as raw bytes |

[mcu-protocol.md](mcu-protocol.md) has everything known about the
protocol: framing, timing, the start-up sequence and every command.

## dspd and dspaudio

dspd copies the DSP firmware (`/usr/lib/osiris/dspaudio.bin`) into the DSP's
memory, starts it and releases the mute lines once it runs. The two share a
block of DSP memory ([dspaudio.h](../services/dspd/dspaudio.h)): the DSP reports its
state and input levels there, dspd writes a gain and a source for each
output.

```sh
ubus call dsp status                              # state, counters, input peaks
ubus call dsp set '{"mute": true}'
ubus call dsp set '{"volume": -600}'              # gain in 0.01 dB, 0 = unity
ubus call dsp set '{"outputs": "all"}'            # front pair or all outputs
```

dspaudio takes the I2S input from the PCM9211 (the tuner and analog inputs
through its ADC, or a digital input) and writes it to the PCM1690 DAC, four
serial lines of two channels each. It is built with dspboot's C6000
toolchain (`make dspaudio`).

## osirisd

What it does today:

- names the system after the receiver model the MCU reports (host name and
  `/data/osiris/model`, unless `/data/etc/hostname` exists)
- makes the receiver show "OSIRIS <build>" as its DSP version
- mutes the DSP while the receiver runs its test tone, which the DSP cannot
  play yet

To try a change, copy the script to `/tmp`, stop the running one and start
the copy from the terminal:

```sh
start-stop-daemon -K -p /var/run/osirisd.pid
/tmp/osirisd
```

## Building

`make buildroot` builds all four into the root file system and the update
bundle, and always rebuilds them, so a change here only needs that.
`make dspaudio` builds the DSP firmware alone.
