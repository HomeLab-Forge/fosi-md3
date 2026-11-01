# Hardware, USB and flash

Everything here was observed on one retail unit (silver) unless marked
**datasheet** or **inferred**. The pin map is in [`stock-firmware.md`](stock-firmware.md).

## Parts

| Part | Role | Limit |
|------|------|-------|
| ESP32-S3R8 (QFN56, rev v0.2), 2 cores at 240 MHz, 8 MB PSRAM | System controller: screen, buttons, menus, photo storage, DAC control, USB mux | Its Wi-Fi and Bluetooth hear nothing on this unit (see [`limits.md`](limits.md)) |
| Boya flash, JEDEC 0x684019, 32 MB, quad I/O | Firmware, the photo volume, settings | |
| GC9A01 round LCD, 240x240, RGB565, colours inverted, PWM backlight | The only display | 1.28 inches |
| Four buttons: Vol+, Vol-, Menu, Vista (the orange one) | All input | No other sensor: no microphone, motion, light or touch sensing in the stock firmware or the GPIO map |
| Savitech SA9312L USB audio bridge | UAC2 up to 384 kHz PCM and native DSD256 | Its firmware appears to be in mask ROM (inferred); the 32 KiB EEPROM holds configuration |
| ESS ES9039Q2M DAC | Conversion; volume, filters and gain set by the ESP32 over I2C | The ESP32 never sees audio samples |
| USB data mux on ESP32 GPIO48 | Gives the port to the ESP32 or to the Savitech | Never both at once |
| Power | Bus-powered from USB; VBUS sensed on an ADC | No battery, no real-time clock: the time is lost at every unplug |

## Two devices behind one port

The MD3 switches its USB data lines between two chips. The user picks one in Menu > USB Mode.

| Mode | Chip | USB ID | What the host sees |
|------|------|--------|--------------------|
| Audio (DAC) | Savitech SA9312L | `262a:1a86`, manufacturer "Speed Dragon", bcdDevice 0x0003 | USB 2.0 high-speed, UAC2 output plus a vendor HID interface |
| Photo Sync (U-Disk) | ESP32-S3, TinyUSB MSC | `303a:4002`, vendor "SD", serial `123456`, bcdDevice 0x0100 | Full speed, one mass-storage interface: a 22.9 MB FAT16 volume labelled `NO NAME` |
| Boot window, about 1.1 s at every power-up | ESP32-S3 ROM USB-Serial-JTAG | `303a:1001`, serial = the chip's MAC | An esptool target |

Switching modes makes the host see one device unplug and another arrive.

### Audio mode descriptors

- Interface 0, HID: only an interrupt IN endpoint (0x81, 32 bytes, bInterval 6). The report
  descriptor (95 bytes) is on Consumer page 0x0C, usage 1. Input is 8 one-bit fields plus 31
  bytes. Output is 16 bytes, sent with `SET_REPORT` on endpoint 0. No feature reports and no
  report IDs. The device sent nothing at idle or during button presses: the buttons never reach
  the computer.
- Interface 1, UAC2 Audio Control: clock source 1 (programmable), input terminal 3 (USB
  streaming, 2 channels), feature unit 10 (master mute, channel 1 and 2 volume, all
  read-write), output terminal 4 (speaker).
- Interface 2, UAC2 streaming. Alt 1, 2 and 3 are PCM at 16, 24 and 32 bits (200, 300 and 400
  bytes per microframe, so up to 384 kHz). Alt 4 is a RAW-format 32-bit setting used for native
  DSD. Asynchronous isochronous OUT with feedback endpoint 0x84.

### Photo Sync mode

- PID 0x4002 is TinyUSB's default for "MSC only", and the descriptors match the ESP-IDF TinyUSB
  MSC example field for field.
- SCSI inquiry: vendor "Fosi Aud", product "Fosi Audio MD3", revision "1.0".
- The volume is FAT16, 22,872,064 bytes, 4096-byte sectors, about 8.2 MB used as shipped.
  Folders: `Photo1-3` (the album), `Game1-3` (the games) and a `System Volume Information`
  folder left by the factory's Windows PC.

## Getting into the ESP32

- **Security:** secure boot disabled, flash encryption disabled, every eFuse key block empty.
  The flash reads out in the clear and the chip boots any image written to it.
- **The boot window.** At every power-up the S3's USB-Serial-JTAG appears for about 1.1 s before
  the stock app moves the mux to the Savitech. A host that polls for the port every 5 ms and
  sends esptool's USB-JTAG reset at once holds the chip in ROM download mode for as long as it
  likes (`tools/catch_rom.py`; caught in under 0.4 s). Fosi calls the bottom port the "firmware
  upgrade" port.
- **The IAP halt.** If Vista (GPIO10) is held at power-up, the stock firmware logs "iap mode
  detect, system halt" and never starts TinyUSB, so the USB-Serial-JTAG stays on the port.
- **Leaving download mode.** A plain RTS hard reset re-enters download mode on this chip.
  `esptool --after watchdog-reset` boots the app. No replug or button is needed.
- **Unbrickable, within limits.** The download path is in mask ROM and opens before any app
  code runs. As long as the bootloader (0x0) and partition table (0x8000) are left alone and no
  eFuse is burned, any app written to 0x10000 can be replaced.

## Flash layout

No OTA slots, so Fosi presumably updates by reflashing through the boot window (inferred). No
public update tool or image exists.

| Label | Type | Offset | Size | Contents as shipped |
|-------|------|--------|------|---------------------|
| nvs | data/nvs | 0x9000 | 24 KB | empty |
| phy_init | data/phy | 0xF000 | 4 KB | empty |
| factory | app | 0x10000 | 9 MB | "ESP32S3_UDAC-DH" v1, ESP-IDF v5.5.1, built 2026-03-26, about 3 MB used |
| storage | data/fat | 0x910000 | 22 MB | the Photo Sync volume, wear-levelled FAT |
| user_dat | data/fat | 0x1F10000 | 24 KB | a raw 24-byte settings record at +0x1000 (magic 0x55) |

Dump a unit with the chip held in download mode:

```sh
python -m esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX \
  --before no-reset --after no-reset read-flash 0 0x2000000 flash-full.bin
```

Read it twice and compare hashes before trusting it. The factory app partition, which
`tools/md3_deploy.py` names in its recovery hint, is a slice of the dump:

```sh
mkdir -p backup && dd if=flash-full.bin of=backup/factory-app.bin bs=4096 skip=16 count=2304
```

## Screen content (works with the stock firmware)

- Every asset is a 240x240 GIF. The album photos are GIF87a at 80 ms per frame, 12 to 112
  frames, up to 1.4 MB. The game assets are GIF89a.
- Games pick a random file from their folder, play it once and hold the last frame:
  Game1 is dice (`1..6.gif`, last frame held about 655 s), Game2 a bottle spin
  (`n/e/s/w.gif`, 240 s), Game3 rock, paper, scissors (120 s).
- So any deck of your own works: a yes/no oracle, a tarot draw, an 8-ball.
- Fosi's web tool only crops images and exports JPG or GIF, with reference limits of 2 MB and
  about 12 fps.
- The stock firmware scans the folders at boot only. New files appear after switching back to
  Audio mode and power-cycling.
- Copy files with macOS AppleDouble files suppressed (`cp -X`, then `dot_clean`): `._*` files
  would land in the image folders. `tools/md3.py push` converts, copies and verifies.
