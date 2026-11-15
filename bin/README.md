# Prebuilt images

Every sample built for each board with nRF Connect SDK v3.4.0, application
and network core merged into one file: `bin/<board>/<sample>.hex`.

| Image | What it is |
|-------|------------|
| `blinky` | RGB LED cycle (LED strip on H1) |
| `hw_test` | Hardware self-test, see [samples/hw_test](../samples/hw_test/README.md) |
| `le_audio_broadcast_source` | Broadcasts the mic as "Troposound R1" / "Troposound H1" |
| `le_audio_broadcast_sink` | Receives a broadcast and plays it on speaker/headphones |
| `usb_audio` | USB Audio Class 2 headset |

Flash a source on one board and a sink on the other.

## Flash

```sh
bin/flash.sh bin/r1/le_audio_broadcast_source.hex 69654557
bin/flash.sh bin/h1/le_audio_broadcast_sink.hex 69654429
```

The serial number is the J-Link's and can be left out with one board
connected. `nrfutil` programs one core per call and rejects the other core's
addresses, so `flash.sh` splits the image at `0x01000000` and programs the
network core, then the application core, then resets.

A board with access protection enabled needs `nrfutil device recover` first,
which erases everything.

## Rebuild

From an nRF Connect SDK shell inside the west workspace:

```sh
bin/build.sh
```
