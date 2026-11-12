# Power: what the MD3 draws and what md3fw changes

The MD3 has no battery. It runs from the phone's USB-C port, so whatever it draws comes out of
the phone's battery. This page puts together what was measured and what the datasheets say.

## Measured: stock firmware against md3fw

The measurements come from one MD3 on an iPhone, with 3.5 mm IEMs at about -40 dB.

| Firmware | MD3's share of the phone battery | Period | Drain from the MD3 |
|---|---|---|---|
| Stock (Fosi, Mar 2026 build), screen always on | 34% of 105% used that day | 3 h 10 min | **about 11.3% per hour** |
| md3fw 0.3.1, screen dark | 37% of 64% used by 09:21 | 3 h 36 min | **about 6.6% per hour** |

That is about **40% less** drain from the MD3. With an iPhone battery of 13-17 Wh, it works out to
roughly 1.5-1.9 W on stock and 0.9-1.1 W on 0.3.1.

**How it was measured.** iOS lists the MD3 under Settings > Battery as "USB-C Accessories", with a
background time.
- **The percentage is a share.** It is the accessory's share of all the battery used in the
  period, not a percentage of the battery's capacity.
- **Converting it.** The drain is share × the period's total use ÷ the accessory's time. For
  0.3.1 that is 37% × 64% ÷ 3.6 h.
- **A cross-check.** The phone's own battery level, logged over the same morning, fell about 12%
  per hour in total with its screen off. That leaves about 5.4% per hour for the phone itself,
  which is plausible.

**Caveats.**
- These are two days with different use, read from iOS's attribution, not a power meter.
- The battery level comes in 5% steps.
- md3fw 0.3.2's idle savings (below) have not been measured yet.

A published review measured the stock firmware on a Samsung S10+ at about 0.5% more battery per
10 minutes than without the MD3, and about 0.35% with the MD3's screen off. That is roughly
0.3-0.5 W, lower than here. Its method was 1% steps over 10-minute runs.

## Where the power goes

| Part | Draw | Source | Under firmware control |
|---|---|---|---|
| Screen (GC9A01 panel, backlight) | the largest single saving from stock to 0.3.x | measured difference above | Yes: dark until Menu, panel asleep |
| ESP32-S3 | about 0.1 W at 160 MHz with frequency scaling (estimate) | ESP-IDF figures | Partly: radios off, clock lowered |
| ES9039Q2M DAC | 54 mW playing; 15 mW muted, with automute and clock gearing | datasheet v0.2.3, p.52-53 | Partly: automute, clock gearing |
| 4 × ES9603Q headphone amps | 2.6 / 4.3 / 8.3 mA each (Low / Normal / Best bias); under 5 µA powered down | datasheet v0.4.1, p.5-6 | On/off only (AMP_PDB, taken to be GPIO17) |
| Savitech SA9312L USB audio bridge | not published; likely the largest remaining part (estimate) | none | No |
| Voltage regulators and converters | losses on all of the above | none | No |

**Amp bias.** Four ES9603Qs come to about 57 mW at Normal bias and about 110 mW at Best. Fosi's
0.00075% THD+N spec for 3.5 mm suggests Normal or Best. The bias pins (BIAS_CTRL1/2) appear to be
strapped on the board: every ESP32 output in the stock pin map is accounted for. So the bias
can't be lowered from firmware without board-level evidence.

**Output power is negligible.** With sensitive IEMs at -40 dB, the power into the earphones is in
the microwatts. Volume and gain (digital in any case) make no measurable difference. The draw is
fixed overhead.

## What md3fw changes, by version

| Version | Change | Effect |
|---|---|---|
| 0.3.0 | Wi-Fi and Bluetooth never started | Saves the radio's idle current |
| 0.3.0 | Screen dark until Menu: backlight off, GC9A01 sleep-in, LVGL stopped | The biggest measured saving |
| 0.3.0 | 160 MHz with frequency scaling down to 80 MHz (stock: 240 MHz fixed); UI task wakes every 250 ms instead of 40 ms | A smaller ESP32 share |
| 0.3.2 | DAC automatic clock gearing on (stock: off) | Lower DAC draw, by the datasheet |
| 0.3.2 | DAC automute on after about 6 s of silence, outputs ramped to ground (stock: off) | 54 → 15 mW for the DAC in silence |
| 0.3.2 | Headphone amps powered down after 30 s of silence or no stream | About 60-110 mW in silence |

The 0.3.2 changes only help while nothing plays: paused, between tracks, or plugged in and
forgotten. During playback the remaining 1 W or so is mostly the USB bridge, the amps' idle
current and the power supplies, and firmware can't switch those off while playing.

## On the device

From 0.3.3 the status screen shows an estimate of the average draw since plug-in, and `md3ctl info`
reports it under `power_est`. The firmware keeps the exact time since boot spent with the screen
on, the amps off, the DAC automuted (or without a stream) and a stream present, and multiplies
each by a fixed figure:

| Part | mW |
|---|---|
| Fixed: USB bridge, ESP32, converters | 706 |
| DAC playing / automuted | 54 / 15 |
| Headphone amps on / off | 80 / 0 |
| Screen on | 150 |

The chip figures are from the datasheets above. The fixed part is calibrated so the model gives
the iOS reading of 0.3.1, 6.6 %/h of an iPhone 15 Pro's 12.7 Wh while playing with the screen
dark. The time shares are exact; the watts are modelled, about ±25%. %/h is the average in W
divided by 12.7 Wh. It cannot see a part that draws more or less than its figure, such as the amp
bias, so it checks the time shares against a measurement rather than replacing one.

## Still to measure

- **0.3.2's drain, playing and paused,** read the same way from iOS.
- **The idle draw:** one hour connected and paused, screen off, on 0.3.1 against 0.3.2.
