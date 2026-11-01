# The stock ESP32-S3 firmware, reverse engineered

Source: the factory app partition (offset 0x10000) of one retail unit: "ESP32S3_UDAC-DH",
ESP-IDF v5.5.1, built 2026-03-26. Read-only static analysis, 2026-10-26. The firmware image itself
is Fosi's and is not included here; dump your own unit to follow along (see `hardware.md`).

**Method.** All findings come from a small recursive-descent Xtensa disassembler built on
capstone 6, with its own decoder for the narrow instructions capstone gets wrong (not included).
Function names were recovered from ESP-IDF log strings and call patterns. Struct layouts were
checked against the ESP-IDF v5.5.1 headers. Addresses below are the stock image's.

**Address to file offset in `factory-app.bin`:** DROM `0x3c08xxxx` maps to `addr - 0x3c080000`.
IROM `0x420xxxxx` maps to `addr - 0x41d90000`. DRAM `0x3fc9xxxx` maps to `addr - 0x3fc99600 + 0x2608bc`.
FreeRTOS runs at 100 Hz, so one tick is 10 ms. Evidence: the source text
`500 / portTICK_PERIOD_MS` compiles to the constant 50 at `0x4200c1d6`.

## Pin map

| GPIO | Config | Function | Confidence |
|---|---|---|---|
| 11 / 12 | SPI2 MOSI / SCLK | GC9A01 LCD, 80 MHz, mode 0, no MISO | high |
| 14 / 13 / 16 | out | LCD CS / DC / RST | high |
| 15 | LEDC ch0, timer0, low-speed, 5 kHz, 10-bit | LCD backlight. Duty is `pct*1023/100`, brightness is `(param[7]+1)*20 %` | high |
| 0 | in, pull-up, active low | **Vol+** (click, and hold-repeat). Also the BOOT strap pin | high |
| 9 | in, pull-up, active low | **Vol−** (click, and hold-repeat) | high |
| 8 | in, pull-up, active low | **Menu/enter** (click opens settings; long press locks the Fkey) | high (behaviour) |
| 10 | in, pull-up, active low | **"Fkey"**: click opens the album/screensaver, double-click rotates, long press turns the screen off. Held at boot: IAP halt. The Vista button (holding Vista at plug-in produces this halt) | high |
| 45 / 46 | I2C1 master SDA / SCL, internal pull-ups, 100 kHz, glitch 7 | ES9039Q2M at 7-bit **0x48** | high |
| 41 / 42 | I2C0 **slave** SDA / SCL, own address **0x6B** (7-bit) | Link from the Savitech | high |
| 48 | out | **USB mux**: 0 = ESP32 (MSC/USJ), 1 = Savitech (DAC) | high |
| 18 | out | UAC strap to the Savitech: 0 = UAC 2.0, 1 = UAC 1.0 | high (logic), medium (target) |
| 47 | out | "usb src" select, driven from GPIO6 and charge state (see §4) | medium |
| 38 | out | ES9039 reset/enable: pulled low 50 ms, then high, before chip-ID read | high |
| 4 | out | Set high at the start of power-up, never cleared. Probably DAC_PWR | medium-low |
| 3 | out | Low during power-up, high after DAC init. Probably an analog/amp rail | low |
| 17 | out | Goes high about 100 ms after DAC init. Probably amp enable/unmute | low |
| 7 | out-low, then input | Held low through power-up and released (Hi-Z) about 150 ms later. Probably Savitech reset | low |
| 5 | ADC1_CH4, 12 dB atten, 12-bit | VBUS sense, vBus = mV × 11 | high |
| 6 | in, no pull | Unknown detect input, read by the ADC task and by usb src | medium |

Unused by the app: UART0 43/44 (console), 19/20 (native USB), 33–37 (octal PSRAM).

## 1. Pin map evidence

- **GPIO setup**, `bsp_gpio_init` at `0x4200af44`, is the first call in `app_main`. It runs
  `gpio_config` (`0x42057890`) on three structs:
  - the struct at `0x3c093bc8` sets pins {0,8,9,10} as input with pull-up;
  - an on-stack mask `0x00018040_00060018` sets {3,4,17,18,38,47,48} as output;
  - mask `0x60` sets {5,6} as input with no pull.

  `0x4200af08` then makes GPIO7 an output at 0. After that the function sets GPIO38 to 0 and
  then 1, sets 47, 48, 4 and 3 to 0, and waits 1 tick. Confidence high.
- **LCD**, `bsp_display_new` at `0x4200b144`.
  - `spi_bus_config_t`: mosi=11, miso=−1, sclk=12, max_transfer=38400 (80 lines).
    It is passed to `spi_bus_initialize(SPI2_HOST, …, SPI_DMA_CH_AUTO)`.
  - `esp_lcd_panel_io_spi_config_t`: cs=14, dc=13, mode 0, pclk 80 000 000,
    queue 10, 8/8-bit cmd/param.
  - Panel config: reset=16, BGR, 16 bpp. Then `mirror(false,false)`, `invert_color(true)`,
    `disp_on`.
  - LVGL port (`0x4200b48c`): 240×240, RGB565, 19200-px double buffer, `swap_bytes` set.
  - Confidence high.
- **Backlight**, `0x4200b068`/`0x4200b0c0`. The timer struct at `0x3c093bf4` is
  `{LOW_SPEED, 10-bit, TIMER_0, 5000 Hz}`. The channel struct is gpio 15, channel 0.
  Confidence high.
- **Buttons**, `my_buttons_init` at `0x4200be0c`. It calls `iot_button_create` four times
  (GPIO 0, 8, 9, 10) with `button_config_t.type=GPIO` and `active_level=0`.
  - All four buttons register SINGLE_CLICK and LONG_PRESS_START.
  - GPIO 0 and 9 also register LONG_PRESS_HOLD, which repeats the volume step.
  - GPIO10 also registers DOUBLE_CLICK, with its short-press time set to 300 ms.
  - Callbacks queue `{type 0=click, 1=long, 2=hold, 3=double; gpio}` to `ui_key_handler`
    at `0x4200ccc0`.
  - Confidence high.
- **I2C master**, `0x4200b004`: `{port 1, sda 45, scl 46, clk XTAL, glitch 7, internal pullup}`.
  The device config at `0x4200c33c` is `{7-bit, 0x48, 100 kHz}`. Confidence high.
- **I2C slave**, `my_slave_i2c_init` at `0x4200bf80`. It uses the slave driver v2 struct
  `{port 0, sda 41, scl 42, clk XTAL, send 100, recv 100, addr 0x6B, 7-bit}`. The callbacks at
  `0x3c093cb8` are `on_request=0x4200bf5c` and `on_receive=0x4200bf2c`. Confidence high.
- **ADC**, `adc_monitor_task` at `0x4200b804`. It calls
  `adc_oneshot_config_channel(ADC1, ch 4, {12 dB, 12-bit})` with curve-fitting calibration.
  The log `vBus:%dmV` prints `mV*11`. Confidence high.
- **Strapping.** GPIO0 (Vol+) is the BOOT strap. Holding Vol+ while plugging in should enter
  ROM download mode, but only if GPIO46 (I2C SCL) reads low at reset. This is untested;
  external pull-ups would defeat it. GPIO45 is the VDD_SPI strap.

## 2. ES9039Q2M driver (`main/dac_chip/src/dac.c`)

**Init** (`dac_init`, `0x4200c30c`, high):
1. Create the I2C bus and add the device at 0x48.
2. Pulse GPIO38: 0, wait 50 ms, 1, wait 50 ms.
3. Run `es9039_init_regs` (`0x4200c3c4`). It reads register **0xE1** up to 10 times and
   expects **0x63**. If it never matches, it logs "ESS9039 not found!".
4. Write the 10 pairs from DRAM `0x3fc99b18`, in order, then read each one back and log it.
5. Write the filter, then apply volume and gain.

| # | reg | val |
|---|---|---|
| 1 | 0x01 | 0xB7 |
| 2 | 0x05 | 0x00 |
| 3 | 0x1D | 0x40 |
| 4 | 0x40 | 0x00 |
| 5 | 0x41 | 0x21 |
| 6 | 0x39 | 0x41 |
| 7 | 0x7B | 0x00 |
| 8 | 0x7D | 0x00 |
| 9 | 0x7C | 0x00 |
| 10 | 0x00 | 0x02 (written last, likely the enable) |

Bit meanings are not decoded here. The open firmware replays these verbatim.

Each write is a 2-byte `i2c_master_transmit([reg,val])`. Reads are `transmit_receive(reg, 1 byte)`.

**Volume** (`dac_volume_apply`, `0x4200c228`, and `dac_write_volume_regs`, `0x4200c4ac`, high):
```
att  = vol_table[param.vol]                            // 101 bytes at DROM 0x3c093d00
if (param.volType == 1)  att += host_mute ? 0xFF : host_att(host_vol)   // per channel
att  = min(att, 0xFF)
if (param.gain == 0 /*Low*/) att = (att > 0xF2) ? 0xFF : att + 12       // -6 dB
write 0x4A = attL ; write 0x4B = attR                   // 0.5 dB/step, 0xFF = mute
```
- `vol_table`: index 0 = 255 (mute), 1 = 192. From 2 to 26 it falls by 4 (184 down to 88,
  2 dB steps). From 26 to 40 it falls by 2 (to 60, 1 dB steps). From 40 to 100 it falls by 1
  (to 0, 0.5 dB steps). The default is 100 on first boot and 26 after a factory reset.
- `host_att` (`0x420719c4`) treats the host byte as signed dB. 0 gives 0. 0xFF..0xBA
  (−1..−70 dB) gives `(256−v)*2`. Anything else gives 0xFF.
- Writes are skipped when nothing changed (cache at `0x3fc9e7d8`/`d9`).

**Gain** is digital only: Low = +12 steps (−6 dB). No GPIO is involved. High confidence.

**Filter** (`0x4200c510`): writes `reg 0x58 = menu index`. The menu table at `0x3c0ae82c`
lists, in order:
0 Minimum phase, 1 Linear fast roll-off apodizing, 2 Linear fast,
3 Linear fast low-ripple, 4 Linear slow, 5 Min-phase fast, 6 Min-phase slow,
7 Min-phase slow low-dispersion.
That makes **8 filters**, plus a "Back" item. Write: high. Filter names: medium.

**DSD/PCM**: the firmware never writes a format-dependent DAC register. The DAC's own
auto-detect handles DSD versus PCM, and the format is used only for the on-screen badge. High.

## 3. I2C slave protocol (Savitech → ESP32)

`on_receive` copies `{buf,len}` and queues it. `i2c_slave_task` (`0x4200bed4`) then does
`regfile[buf[0]] = buf[1]` (regfile at `0x3fc99ae0`) and logs `rec:0x%02x,0x%02x`. Only the
first two bytes of a write are used. Each write is one register/value pair.

`on_request` queues an event that the task ignores. **Nothing flows back**: a master read
gets no data. High.

`misc_task` (`0x4200c568`) runs every 40 ms. Its call to `slave_poll_update` (`0x4200c180`)
consumes these registers:

| reg | use |
|---|---|
| 0x10 | bit1 = DSD stream |
| 0x11 | PCM rate code (low 7 bits). 0x40 or 0x0A = 44.1k, 0x00 = 48k, 0x48 = 88.2k, 0x08 = 96k, 0x58 = 176.4k, 0x18 = 192k, 0x78 = 352.8k, 0x38 = 384k. With DSD and reg 0x1E bit0 clear: 0x58 = DSD64 and 0x78 = DSD128 (DoP) |
| 0x1E | native DSD: `(v&0x71)` = 0x11 → DSD64, 0x21 → DSD128, 0x41 → DSD256 |
| 0x20 / 0x21 | host volume L / R (signed dB, see §2) |
| 0x22 | 1 = host mute |

After each update the firmware re-applies volume and redraws the 70×17 rate badge. The 13
badges run from PCM 44.1k to DSD512; 768k and DSD512 are never selected. Host volume and mute
are applied **only when Volume Type = Enable**. High confidence.

**HID.** Whether the Savitech forwards its 16-byte HID OUT reports onto this bus is not
visible from the ESP32 side. If it does, they appear as `rec:` lines on the console (UART0 TX,
GPIO43). As shipped, the link carries only volume, mute and rate; it cannot drive the UI.

## 4. Boot flow and modes

`app_main` (`0x4200a97c`), high:
1. `bsp_gpio_init`: GPIO48 = 0, so the USB port goes to the ESP32; GPIO7 is held low; the
   DAC reset is pulsed.
2. Wait 10 ms.
3. **If `gpio_get_level(10) != 1`** (Fkey held), log "iap mode detect, system halt", call
   `dac_msc_set(1)` (GPIO48 = 0) and loop `vTaskDelay(100)` forever. TinyUSB is never
   installed, so the **USB-Serial-JTAG stays on the port indefinitely**. Hold GPIO10 at
   plug-in to get a stable esptool target.
4. Log "密钥验证成功…" ("key verified"). This is unconditional: no key or MAC check exists, and
   the MAC log string is never referenced.
5. `user_param_load`, then `state_init_from_param`.
6. `usb_msc_init`: mount FAT "storage" at `/disk`, wait 100 ms, then
   `tinyusb_driver_install` with a default descriptor. This always runs, and it is where the
   USB-Serial-JTAG disappears.
7. `my_slave_i2c_init`.
8. `audio_power_on_sequence` (`0x4200b6bc`):
   1. GPIO3 = 0, GPIO17 = 0, GPIO7 low, GPIO4 = 1.
   2. `usb_src_set`, `uac_mode_set(param[1])` on GPIO18, `dac_msc_set(param[6])` on GPIO48.
   3. `dac_init`.
   4. GPIO3 = 1. A countdown in `adc_monitor_task` (50 ms per step) later sets GPIO17 = 1
      (+100 ms) and releases GPIO7 to input (+150 ms).
9. Display and LVGL setup, then buttons and UI (tasks `ui_task`, `misc_task`, a 50 ms
   screensaver timer), then the ADC monitor.

**Mode switch** (settings parent 0, `ui_settings_apply` at `0x4200c8b0`): set `param[6]`,
call `dac_msc_set`, save, wait 50 ms, then rerun the whole power-on sequence. U-disk mode
only drops GPIO48. The ESP32's MSC is always live; the mux decides who owns the port.
UAC changes rerun the sequence the same way.

**ADC task, every 50 ms:**
- VBUS > 5.5 V: shows a "VBUS overflow!" label only. Despite the log text, it never touches
  a pin.
- VBUS > 3.85 V and GPIO6 = 1: state "charging", rerun power-on once.
- VBUS ≤ 3.85 V: "only usb pd in", rerun power-on when coming from the charging state.
- `usb_src_set` sets GPIO47 = 0 while charging; otherwise GPIO47 copies GPIO6.
- The hardware meaning of GPIO6 and GPIO47 is unknown (medium on the logic, low on purpose).

## 5. Settings record (`user_dat` + 0x1000, 24 bytes)

`user_param_load` at `0x4200baf8` uses `esp_partition_find_first(data, 0x81, "user_dat")`
and reads offset 0x1000. If byte 0 is not 0x55 it loads defaults and saves. Saves (`0x4200ed90`)
erase the 4 KB sector and write 24 bytes, with no CRC. `misc_task` saves 50 × 40 ms = 2 s
after the last change. High confidence.

| off | field | values | first-boot / reset | dump |
|---|---|---|---|---|
| 0x00 | magic | 0x55 | 0x55 | 0x55 |
| 0x01 | UAC | 0 = 2.0, 1 = 1.0 | 0 | 0 |
| 0x02 | gain | 0 = Low (−6 dB), 1 = High | 1 | 1 |
| 0x03 | filter | 0–7 → reg 0x58 | 0 | 0 |
| 0x04 | volume type | 0 = ignore host volume, 1 = apply host volume and mute | 0 | 0 |
| 0x05 | usb src | only logged | 0 | 0 |
| 0x06 | DAC/U-disk | 0 = DAC, 1 = U-disk | 0 | 0 |
| 0x07 | brightness | 0–4 → 20–100 % | 2 | 2 |
| 0x08 | rotation | 0–3 → 0/90/180/270° | 0 | 0 |
| 0x09 | saver timeout | 0 off, 10/20/30/60 s | 1 / 0 | 0 |
| 0x0A | saver source | 0 off, 1 custom | 1 | 1 |
| 0x0B | saver poll | 0 off, 10/20/30 s | 1 / 0 | 3 |
| 0x0C | language | 0 EN, 1 ZH, 2 JA | 1 / 0 | 0 |
| 0x0D | volume | 0–100 | 100 / 26 | 15 |
| 0x0E | volume copy (R) | always = 0x0D | same | 15 |
| 0x0F | album folder | 0–5 = Photo1–3, Game1–3 | 0 | 2 |
| 0x10 | ask-language flag | 1 after factory reset | 0 / 1 | 0 |
| 0x11–13 | padding | never written | – | ff |
| 0x14 | u32 | unused | 0 | 0 |

## 6. UI

**Screens.** `ui_state` is at `0x3fc9e80c+0x98`: 0 = main, 1 = album/screensaver,
2 = settings, 3 = sub-settings. The UI event queue carries:

| event | action |
|---|---|
| 1 | main / exit |
| 2 | settings |
| 3 | sub-settings |
| 4 | screensaver |
| 5 | next image |

On boot the firmware goes to main. If `param[0x10]` is set it goes to the Language page
instead.

**Keys** (`ui_key_handler`, high):

| key | action |
|---|---|
| Vol± click or hold | Change the volume on main and album. In menus, move up and down, with the direction swapped at 180/270° |
| GPIO8 click | main → settings; settings → enter, or exit on "Exit"; sub-settings → apply; album → main |
| GPIO8 long press | Main or album: toggle the Fkey lock and its lock icon. Menus: exit to main. With Fkey also held: **factory reset** |
| Fkey click | main → album; album → next image; settings → main; sub-settings → settings |
| Fkey double-click | Main: rotate 90°. Album: next folder |
| Fkey long press | Backlight off |

**Settings.** There are 14 parents. The table at DROM `0x3c0ae82c` (stride 0x74) holds the
item counts and English labels. Menus render as RGB565 images per language
(136×26 items, 112×28 titles).

**Folder scan** (`scan_image_folder`, `0x4200e574`):
- Covers `/disk/Photo1-3,Game1-3` at boot only.
- Uses readdir order (unsorted, FAT order). It skips `.`, `..`, dot-files and
  "System Volume Information".
- Accepts `.jpg .jpeg .png .bmp .gif`, case-insensitive, with no count limit.
- **PNG is listed but cannot be shown** ("open picture error").
- JPEG and BMP must be ≤ 240×240. A GIF is read whole into PSRAM, with no size check.
- Photo folders step in order. Game folders (index ≥ 3) pick a file at random (`esp_random`).

**Embedded graphics.** These are LVGL 9 `lv_image_dsc_t` structs, RGB565, cf 0x12, stored
uncompressed, so they can be located by scanning for valid descriptors.

| asset | where | notes |
|---|---|---|
| Main background (VU scale 0–100, "VOL") | data at DROM `0x3c0af28c` | 240×240, 115 200 B |
| Volume needle position | `0x3c0ae504` | 101 × {float angle in 0.1°, int16 x, int16 y} |
| Rate badges | 70×17 | 13 pointers at DRAM `0x3fc9a28c` |
| Filter badges F1–F8 | 17×17 | `0x3fc9a314` |
| Gain badges L/H | 17×17 | `0x3fc9a334` |
| UAC badges U2/U1 | 17×17 | `0x3fc9a33c` |
| U-disk icon | 13×14 | `0x3c0cb4c4` |
| Lock icon | 11×14 | `0x3c0cb630` |

## Unknowns

1. The bit meanings of the ES9039 init writes.
2. The hardware roles of GPIO3, 4, 6, 7, 17 and 47, which are inferred from sequencing
   only. A multimeter on the board, or a UART0 log during plug-in, would settle them.
3. Whether the Savitech forwards host HID onto I2C 0x6B.
4. Whether Vol+ (GPIO0) at plug-in reaches ROM download mode, since that depends on GPIO46 at
   reset. IAP (GPIO10) is the known-good path.
