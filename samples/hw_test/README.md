# Hardware test

Board bring-up self-test for R1 and H1. Runs every test at boot, prints a
PASS/WARN/FAIL summary on the UART console (115200), then drops into a shell.
RGB LED ends green when nothing failed, red otherwise.

## Build and flash

```sh
west build -p -b r1/nrf5340/cpuapp samples/samples/hw_test
west flash --softreset
```

## Tests

| Test | Board | Checks | Needs you |
|------|-------|--------|-----------|
| leds | both | red, green, blue, white | watch |
| clocks | both | 32.768 kHz LFXO and 32 MHz HFXO start | |
| i2c | both | TAC5211 @0x50 ACKs, MAX17048 @0x36 version 0x001x | |
| fuel_gauge | both | battery voltage and state of charge via driver | |
| audio | both | I2S at 48 kHz, mic noise floor, 1 kHz tone from speaker picked up by the mic | listen |
| codec_gpio | both | TAC5211 GPO1 -> MCU and MCU -> TAC5211 GPI1 | |
| usb | both | VBUS detected and the host enumerates the board | USB to a computer |
| radio | both | net core boots, BLE scan hears devices, then advertises `Troposound <board> HWTEST` (nRF21540 at 20 dBm, PCB antenna) | optional: find it in a phone scanner |
| led_strip | H1 | 32 WS2812 red/green/blue/white, VLED fault pin idle | watch |
| qwiic | H1 | SDA/SCL pulled up, scan for devices (WARN if none attached) | optional: attach a Qwiic device |
| gauge_alert | both | MAX17048 ALRT pin idle | |
| buttons | both | every `/buttons` key (R1: PTT; H1: power, mute, smart) | press each |
| encoder | H1 | quarter turn | turn |
| pot | R1 | volume pot sweeps over 70 % of range | turn fully both ways |
| jack | R1 | headphone jack detect changes | plug or unplug |
| charging | both | USB VBUS, BQ24075 `/CHG` and MAX17048 (mV, %, %/h) agree; on USB plug-in the charger starts and the battery voltage rises by 15 mV or more, proving charge current flows. With the debugger attached this ends in WARN, see [Charge LED](#charge-led-without-the-debugger) | unplug USB, plug it back in |
| rf_audio | both | mic noise with the radio off vs. busy (fast advertising + continuous scan); WARN at +3 dB or more. Then 15 s mic -> speaker/headphones loop with the radio busy | listen for buzz; any button ends early |
| power_off | H1 | LTC2950 KILL turns the headset off. Only with `hw run power_off` | |

Interactive steps wait 10 s each.

## Shell

```
hw list          # list tests
hw run           # run all again
hw run audio     # run one
hw run rf_audio  # listen for radio noise in the audio path again
hw battery 60    # USB, /CHG, battery mV, %, %/h once per second for 60 s
```

hw_test turns the MAX17048 hibernate mode off (hibernating, it measures only
every 45 s); the gauge keeps that until its supply is removed. Charge rate in
%/h needs a minute or two to settle after a change.

## Charge LED (without the debugger)

The debugger connector disables charging in hardware, so test charging with
it removed. Wait until the boot test run has finished (the RGB LED shows the
steady green/red summary), then plug or unplug USB. From then on the
RGB LED shows the charge state:

| LED | Meaning |
|-----|---------|
| blue blinking 1 Hz | charging; the battery voltage rose when the charger started, so current flows |
| red blinking 2 Hz | fault: `/CHG` says charging but the battery voltage did not rise, or `/CHG` active without USB |
| blue steady | USB in, charger off (battery full) |
| short flash every 2 s | on battery: green 50 % or more, yellow 20 to 49 %, red below 20 % |

R1 has its own charge LED driven by the charger, so there the RGB LED shows
only the fault and the battery level. Running any test returns the LED to
the tests until USB is plugged or unplugged again.
