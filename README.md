# Troposound samples

## Setup

```sh
west init -m https://github.com/Troposound/samples ncs
cd ncs
west update
```

## Build

```sh
west build -b h1/nrf5340/cpuapp samples/samples/blinky
west flash --softreset
```

## Prebuilt images

Every sample built for each board, both cores merged, is attached to each
[release](https://github.com/Troposound/samples/releases). See
[bin/README.md](bin/README.md).

## Boards

| Board | Target |
|-------|--------|
| H1 | `h1/nrf5340/cpuapp` |
| R1 | `r1/nrf5340/cpuapp` |

## Samples

| Sample | Boards | What it shows |
|--------|--------|---------------|
| `blinky` | H1, R1 | RGB LED cycle, LED strip on H1 |
| `hw_test` | H1, R1 | Self-test of every peripheral with PASS/FAIL summary and `hw` shell |
| `le_audio_broadcast` | H1, R1 | LE Audio broadcast (Public Broadcast Profile): mic to broadcast source, or sink to speaker/headphones |
| `usb_audio` | H1, R1 | USB Audio Class 2 headset (48 kHz stereo out, mono mic in) |
| `usb_broadcast` | H1, R1 | USB Audio Class 2 speaker broadcast as LE Audio in stereo (left and right BIS), received by the `le_audio_broadcast` sink |

Shared code lives in the module: `drivers/audio/tac5211` (codec driver),
`lib/audio_i2s` (I2S0 master at exactly 48 kHz from the audio PLL),
`lib/headset` (headset mode on the R1 jack) and `lib/volume` (R1 volume knob,
H1 rotary encoder, driving the codec or a digital gain).

The board files run the nRF21540 front-end module at 20 dBm (POUTA gain)
and always use the PCB antenna (ANT_SEL held low). Every Bluetooth sample
inherits this.

## License

[Apache-2.0](LICENSE)
