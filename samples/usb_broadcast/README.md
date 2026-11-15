# USB broadcast

A USB speaker that broadcasts what the host plays as LE Audio. Pair it with
`le_audio_broadcast` built as a sink, which plays the broadcast on its
speaker or headphones.

```
host -> USB Audio Class 2 (48 kHz 16-bit stereo) -> LC3 48 kHz per channel -> two BISes
```

Left and right go out as two BISes in one subgroup, tagged front left and
front right, each BAP `48_4_1` (10 ms frames, 120 octets). The broadcast is
advertised as a high quality Public Broadcast named `Troposound R1 USB` /
`Troposound H1 USB`. The broadcast runs from boot and sends silence while
the host is not playing, so sinks stay synced.

## Clock synchronisation

The BIG sets the pace: each left/right pair the controller sends releases
the next one, so the encoders take exactly one 10 ms frame per BIG event.
There is no local audio clock and no IN stream, so the USB OUT endpoint is
asynchronous with an explicit feedback endpoint (implicit feedback needs
an IN data stream). The feedback holds the playback ring at 5 ms just after
each frame is taken, so the host follows the BIG clock and no samples are
dropped or repeated. A host that ignores feedback still plays; the ring
then drifts and recovers with one short gap every few minutes.

Windows' UAC2 driver expects a 4-byte full-speed feedback value. For
Windows hosts add `CONFIG_USBD_UAC2_FS_WINDOWS_WORKAROUND=y`. macOS and
Linux work with the spec-compliant default.

## Controls

| Board | Control |
|-------|---------|
| R1 | Volume knob sets the broadcast level from its position, -60 to +6 dB |
| H1 | Rotary encoder, 1 dB per detent, starts at 0 dB |
| Both | Green LED on once the broadcast and USB are up |

The host's own volume applies as well. The R1 speaker amplifier stays off.

## Building

```sh
west build -p -b h1/nrf5340/cpuapp samples/samples/usb_broadcast
west flash --softreset
```

| Option | Default | Notes |
|--------|---------|-------|
| `CONFIG_BROADCAST_NAME` | `Troposound <board> USB` | Broadcast_Name |
| `CONFIG_APP_USB_PRODUCT` | `Troposound <board> Broadcast` | Name the host shows |
| `CONFIG_APP_USB_PID` | `0x530D` | |

Twister: `sample.troposound.usb_broadcast`.

## Testing

1. Flash `usb_broadcast` on one board and `le_audio_broadcast` built with
   `sink.conf` on the other.
2. Connect the broadcaster over USB. It appears as an output device named
   `Troposound <board> Broadcast`.
3. The sink finds `Troposound <board> USB`, logs `BIS left started` and
   `BIS right started`, and turns green. Play audio on the host; it comes out
   of the sink in stereo.
4. While the host plays, the broadcaster logs every 10 s:
   `fill=<samples> feedback=<ppm> underruns=0 overruns=0`. `fill` stays
   near 240 and `feedback` settles within a few hundred ppm.
