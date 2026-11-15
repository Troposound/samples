# Blinky

Cycles the RGB LED red, green, blue every 500 ms. On H1 it also drives the
32-LED WS2812 strip through the effects rainbow, scanner, police, disco and
breathe, 8 s each, with brightness capped at 40/255 to stay within the VLED
current limit. A VLED overcurrent fault switches the strip supply off and
retries after 2 s.

## Build and flash

```sh
west build -p -b h1/nrf5340/cpuapp samples/samples/blinky
west flash --softreset
```
