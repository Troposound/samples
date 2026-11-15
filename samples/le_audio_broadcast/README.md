# LE Audio broadcast

LE Audio broadcast for the Troposound R1 and H1 (nRF5340 + nRF21540 FEM +
TAC5211 codec). One sample, two roles selected at build time:

| Role | What it does |
|------|--------------|
| Broadcast source (default) | TAC5211 microphone (48 kHz) → LC3 mono, 10 ms frames → one BIS. Advertised as an unencrypted Public Broadcast named `Troposound R1` / `Troposound H1`. |
| Broadcast sink (`sink.conf`) | Scans for a broadcast source, syncs to its first BIS, decodes LC3 to 48 kHz and plays it on both output channels. |

The app core runs the Zephyr LE Audio host and liblc3 at 128 MHz. The net core
runs NCS `ipc_radio` (configured in `sysbuild/ipc_radio/prj.conf`), shared by
both roles.

## Building

```sh
# Source
west build -p -b r1/nrf5340/cpuapp -d build_src samples/samples/le_audio_broadcast

# Sink
west build -p -b h1/nrf5340/cpuapp -d build_sink samples/samples/le_audio_broadcast -- \
  -DEXTRA_CONF_FILE=sink.conf

west flash -d build_src --softreset
```

Both boards build both roles. Options:

| Option | Default | Notes |
|--------|---------|-------|
| `CONFIG_BROADCAST_NAME` | `Troposound <board>` | Source: Broadcast_Name |
| `CONFIG_BROADCAST_SAMPLE_RATE_24K` / `_48K` | 24K | Source: LC3 24_2_1 or 48_2_1 |
| `CONFIG_BROADCAST_TARGET_NAME` | `""` (any) | Sink: only sync to names containing this |
| `CONFIG_BROADCAST_VOLUME_HALF_DB` | -20 (-10 dB) | Sink: start-up volume with the H1 encoder |

Twister: `sample.troposound.le_audio_broadcast.source` and `.sink`.

## Controls

| Control | R1 | H1 | Source | Sink |
|---------|----|----|--------|------|
| Mute toggle | PTT button | Mute button | Microphone muted (silence is sent) | Output muted |
| Volume | Volume knob, -60..+6 dB | Rotary encoder, 1 dB per detent | - | Output volume |
| TRRS headset | Headphone jack | - | Headset mic instead of the built-in mic | Headset instead of the speaker |

Headset mode follows the R1 jack in both roles: capture switches to the
headset mic (IN2P) with 30 dB more gain (6 dB in the codec, 24 dB in
software into a soft clipper), the speaker EQ is bypassed and the speaker amplifier stays
off. Without a headset the amplifier is on only while a BIS is playing.

## LEDs

| LED | Meaning |
|-----|---------|
| Blue blinking | Advertising (source) / scanning (sink) |
| Green | Streaming |
| Yellow | Streaming, muted |
| Red | Error |

## Manual test

1. Flash the source build on an R1 and the sink build on an H1.
2. Source: blue blinks, then green. The log shows `Advertising "Troposound R1"`
   and `Sent N SDUs` every 10 s.
3. Sink: blue blinks, then the log shows `Found "Troposound R1"` and
   `BIS started: LC3 24000 Hz, 60 octets`, and the LED turns green. Speak into
   the R1 and listen on the H1.
4. Press the R1 PTT button: the source LED turns yellow and the H1 goes
   silent. Press again to unmute. Turn the H1 encoder to change the volume and
   press its mute button.
5. Power-cycle the source: the sink goes back to blue and resyncs when the
   source returns.
6. Swap the roles (H1 source, R1 sink) and repeat; check the R1 speaker plays,
   the knob sets the volume. Plug in a TRRS headset: the speaker goes quiet,
   the audio plays in the headset, and as source the headset mic is sent.

The source is a standard Public Broadcast, so a phone or receiver with LE
Audio broadcast support can also find and join `Troposound R1`.

## Limitations

- Mono: the sink plays the first BIS of a stereo broadcast on both channels.
- Unencrypted broadcasts only; the sink skips encrypted ones.
- 10 ms LC3 frames only.
- No drift compensation between the I2S and ISO clocks: the source drops a
  frame and the sink re-primes (a short gap) when they drift apart.
