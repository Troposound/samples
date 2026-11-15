# USB Audio

USB Audio Class 2 soundcard on the TAC5211, for `r1/nrf5340/cpuapp` and
`h1/nrf5340/cpuapp`. It enumerates as "Troposound R1" or "Troposound H1"
(VID 0x1915, PID 0x530C) and offers:

- **Playback**: 48 kHz, 16-bit stereo, host to codec headphone/speaker output.
- **Capture**: 48 kHz, 16-bit mono, codec ADC channel 1 to host.

How it works: USB OUT feeds a ring buffer that the I2S0 DMA (1 ms blocks)
drains into the codec. The I2S0 DMA fills a mic ring buffer that is sent to
the host once per USB frame (SOF). The I2S clock comes from the 12.288 MHz
audio PLL, not from USB. Capture sends 47, 48 or 49 samples per frame to hold
its ring buffer steady, so the mic stream runs at the I2S rate; both
endpoints use implicit feedback, so the host paces playback from it (the
nRF5340 USBD has only one isochronous IN endpoint). Playback-only streams run
at the host's nominal rate and may drop or repeat 1 ms on long runs.

Output volume comes from the R1 volume knob (-60..+6 dB over its travel) or
the H1 rotary encoder (1 dB per detent, starts at -10 dB). There is no host
volume control (no Feature Unit): the UAC2 class does not answer Feature Unit
control requests, so the host's software volume applies on top.

| | R1 | H1 |
|-|----|----|
| Speaker | Amplifier on while the host plays, off in headset mode | - |
| Microphone | Built-in (IN1P); in headset mode the headset mic (IN2P) with +30 dB more gain | Built-in (IN1P) |
| Headset mode | TRRS headset in the jack: headset mic, speaker off, speaker EQ bypassed | - |

The green LED is on while the soundcard runs.

Log output (UART): USB events, stream start/stop, and once per second while
any stream is open:

```
play=1 mic=0 underruns=0 late=0 play_fill=150 mic_fill=96
```

`underruns` counts I2S blocks that found less than 1 ms of host audio. `late`
counts I2S buffer hand-offs that missed the DMA deadline.

## Building

```sh
west build -p -b r1/nrf5340/cpuapp -d build_usb_r1 samples/samples/usb_audio
west build -p -b h1/nrf5340/cpuapp -d build_usb_h1 samples/samples/usb_audio
west flash -d build_usb_r1 --softreset
```

## Testing manually (macOS)

1. Connect the board's USB port. The log shows `USB: VBUS ready`, then
   `USB: Configured`.
2. Open **Audio MIDI Setup**. "Troposound R1" (or H1) appears with
   2 output channels and 1 input channel at 48 kHz.
3. Playback: run `say -a "Troposound R1" "hello from the mac"`, or pick the
   device as the output in System Settings > Sound and play music. The log
   shows `Playback stream started` and per-second stats; `underruns` should
   stay at 0 once playing.
4. Capture: select the device as the input in QuickTime Player >
   New Audio Recording, record a few seconds while talking, and play it back.
   The log shows `Capture stream started`.
5. R1 headset: plug in a TRRS headset while playing and recording. The log
   shows `Headset in: headset mic, speaker off`, the speaker goes quiet, the
   audio plays in the headset and the recording switches to the headset mic.
   Unplug: `Headset out: onboard mic, speaker` and the speaker plays again.
