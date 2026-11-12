# md3fw: open firmware for the Fosi Audio MD3

Built on the reverse engineering by BenevolentFutures
([github.com/BenevolentFutures/fosi-md3](https://github.com/BenevolentFutures/fosi-md3)), MIT.

The Fosi Audio MD3 is a small USB-C DAC and headphone amplifier with a round 1.28-inch screen. An
ESP32-S3 inside runs the screen, the buttons and the ES9039Q2M DAC. md3fw replaces the app on that
ESP32. It writes only the app partition and keeps the audio path, the photo volume and the
settings format, so the stock firmware can go back on at any time.

Version 0.3.2 is maintained and used daily on one MD3 as a DAC for an iPhone.

![The status screen: Warm preset at 44.1 kHz](images/status-screen.png)

## What it does

- **Audio as stock:** every sample rate and DSD, through the Savitech USB audio bridge. The
  volume uses the stock 101-step curve.
- **One status screen:** the stream ("96 kHz PCM", "DSD128", "no stream"), "High Gain" or "Low
  Gain", the EQ preset's curve, its name and a note, and the volume as a ring with its value in dB.
  The screen stays dark until you press Menu and goes dark again 8 s after the last key.
- **Nine EQ presets** on the DAC's programmable filter: Flat (the built-in filter), Bass+ and
  Bass- (±6 dB below 1 kHz), Mid+ and Mid- (±5 dB around 2 kHz, 44.1/48 kHz only), Treble+ and
  Treble- (±6 dB above 5 kHz), Warm (tilts down to -5 dB at 10 kHz) and Bright (up to +5 dB).
  DSD bypasses the EQ.
- **Gain:** High or Low. Low is 6 dB of digital attenuation, the same as stock.
- **Lower power:** no Wi-Fi or Bluetooth, 160 MHz with frequency scaling down to 80 MHz, the
  screen and its UI stopped while dark, and in silence the DAC automutes and the headphone amps
  power down after 30 s.
- **Photo Sync** (U-Disk mode) works as before, and adds a USB control channel for status,
  screenshots, logs and hands-free reflashing.

## Battery

The MD3 has no battery of its own and runs from the phone. Measured on an iPhone with IEMs, from
the share iOS gives "USB-C Accessories":

| Firmware | Drain from the MD3 | About |
|---|---|---|
| Stock (Fosi) | ~11.3% of the phone battery per hour | 1.5-1.9 W |
| md3fw 0.3.1 | ~6.6% per hour, about 40% less | 0.9-1.1 W |

Most of the saving comes from the dark screen. Most of what is left is the USB audio bridge, the
headphone amps and the power supplies, which have to run while music plays. 0.3.2 also saves
power in silence: the DAC automutes and the amps power down. That part is not measured yet. The
method, the caveats and the power budget per chip are in [`docs/power.md`](docs/power.md).

## Buttons

| Button | Screen off | Screen on |
|--------|------------|-----------|
| Menu, click | Screen on | Screen off |
| Menu, hold 0.8 s | Screen on | Switch DAC <-> U-Disk |
| Vista (front), click | Nothing | Next preset |
| Vista, hold 0.8 s | Nothing | Switch High <-> Low gain |
| Vol+ / Vol- | Volume | Volume, shown big |

Holding Vista while plugging in starts no app and keeps the ESP32's USB recovery path.

## Install

Build with ESP-IDF 5.5.1 in `firmware/` (`idf.py build`). Flashing and dumping are done from
Linux; the tools also work on macOS. In short:

1. Add a udev rule for USB ids 303a:1001 and 303a:4002 so your user can open them.
2. Dump the whole flash: `python tools/catch_rom.py --dump`, then replug the MD3. Do it twice.
3. First flash: `python tools/catch_rom.py --hold`, replug, write `firmware/build/md3fw.bin` to
   0x10000 with esptool, then reset with `catch_rom.py --port`.
4. Later updates: the same as step 3. (`tools/md3_deploy.py` does it hands-free from U-Disk
   mode, but finds the drive on macOS only for now.)

The exact commands, the udev rule, the EQ calibration and the way back to stock are in
[`docs/open-firmware.md`](docs/open-firmware.md).

## Before you try any of it

- **Dump your own unit first** (all 32 MB, read twice) and keep it. You need it to go back:
  no Fosi firmware, art or documents are included here.
- **Write only the app partition at 0x10000.** Never write the bootloader (0x0) or the partition
  table (0x8000), and never burn eFuses. Leave those alone and the unit can't be bricked, since
  download mode lives in mask ROM.
- **Recovery:** if an image fails to start, catch the boot window with `tools/catch_rom.py` and
  write the app again, or restore your dump with `tools/md3_factory_restore.py`.
- **Never send guessed HID output reports to the Savitech.** Unknown vendor commands on these
  bridges can enter an update mode. Only the EEPROM read is documented.
- md3fw runs on one unit. Nothing has been tried on a second one.

## Known issues

- Charger in the pass-through port: before 0.3.2 the iPhone dropped the MD3 until it was
  replugged. 0.3.2 has a fix that is not yet tested.
- Host volume, brightness, rotation and UAC 1.0/2.0 have no control. Their saved values still
  apply; change them with the stock firmware.

## Layout

| Path | Contents |
|------|----------|
| `firmware/` | The ESP-IDF project (source only) |
| `design/status-screen.html` | Interactive mock-up of the status screen |
| `docs/power.md` | Battery measurements and the power budget per chip |
| `docs/open-firmware.md` | md3fw: controls, EQ, control channel, build, flash, restore |
| `docs/hardware.md`, `docs/stock-firmware.md`, `docs/savitech.md`, `docs/limits.md` | Reverse-engineering reference: chips, pin map, protocols, settings record, limits |
| `docs/history.md` | How the original reverse engineering went |
| `tools/` | Python host tools: dump, flash, deploy, restore, control channel, EQ design |
| `tests/host/` | Host-compiled C tests of the firmware's USB, storage, link and audio code |
| `images/` | The status screen |

## License

MIT, see [`LICENSE`](LICENSE). Fosi Audio and MD3 are Fosi Audio's trademarks. This project is
not affiliated with or endorsed by Fosi Audio, Savitech, ESS or Espressif.
