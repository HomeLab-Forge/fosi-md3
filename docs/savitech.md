# The Savitech SA9312L audio bridge

The Savitech chip owns the USB port in Audio mode. Its firmware appears to be in mask ROM
(inferred); its 32 KiB I2C EEPROM holds configuration. We changed nothing on it: no EEPROM write, erase, reset or update
command was ever sent.

## What it tells the ESP32

The Savitech is an I2C master to the ESP32, which listens as a slave at 7-bit address **0x6B**
(GPIO41/42). Each write is one register/value pair. The stock firmware uses:

| Reg | Meaning |
|-----|---------|
| 0x10 | bit 1 = DSD stream |
| 0x11 | PCM rate code (low 7 bits): 0x40 or 0x0A = 44.1k, 0x00 = 48k, 0x48 = 88.2k, 0x08 = 96k, 0x58 = 176.4k, 0x18 = 192k, 0x78 = 352.8k, 0x38 = 384k. With DSD and reg 0x1E bit 0 clear: 0x58 = DSD64 and 0x78 = DSD128 (DoP) |
| 0x1E | native DSD: `(v & 0x71)` = 0x11 is DSD64, 0x21 DSD128, 0x41 DSD256 |
| 0x20 / 0x21 | host volume, left / right, as signed dB |
| 0x22 | 1 = host mute |

Nothing flows back: a master read gets no data. So in Audio mode the screen can know the
sample rate, DSD, and the host's volume and mute, and nothing else about the music. Details in
[`stock-firmware.md`](stock-firmware.md) §3.

## The vendor HID channel

Interface 0 in Audio mode is a HID interface: 16-byte output reports sent with `SET_REPORT` on
endpoint 0, 32-byte input reports on interrupt endpoint 0x81. The device sends nothing at idle
or when its buttons are pressed.

**Do not send guessed output reports.** Unknown vendor commands on USB audio bridges of this
kind can enter an update or ISP mode. The only command documented here is a read.

### EEPROM read (confirmed on the MD3)

No MD3 tool exists. The command was recovered by static analysis of a different product's
Windows updater for the same chip family: FiiO's Q11 V0.13 package, which contains
"SA9312L Write EEPROM (MP) Utility v1.3.0". That updater looks for Savitech VID `262a` with PIDs
`9312`, `9300`, `920e` and `920f`. The MD3 (`1a86`) is not on its list but accepts the read.
**The Q11 firmware payload is not an MD3 image and must never be written to an MD3.**

Request, 16 bytes on the wire (hidapi adds a zero report-ID byte in host memory first):

| Offset | Value | Meaning |
|-------:|-------|---------|
| 0 | `seq` | nonzero transaction sequence |
| 1 | `12` | read transaction selector |
| 2 | `e4` | EEPROM read |
| 3 | `a0` | constant parameter (also the usual I2C address of a 24-series EEPROM) |
| 4 | `addr_hi` | EEPROM address, big-endian |
| 5 | `addr_lo` | |
| 6 | `length` | 1 to 16 bytes |
| 7-15 | `00` | reserved |

Example, eight bytes at address 0 with sequence 1:

```text
01 12 e4 a0 00 00 08 00 00 00 00 00 00 00 00 00
```

Response, as an input report with no report-ID byte:

| Offset | Content |
|-------:|---------|
| 0-5 | not used for validation |
| 6 | the request's sequence |
| 7 | the requested length |
| 8 onward | the data |

The Windows updater's buffers carry a report-ID byte, so its offsets are one higher. A first
guess at the data offset (12) was wrong; a boundary read at `0x7ff0` caught it before any full
dump.

What the reads showed:
- The EEPROM is 32 KiB: address `0x8000` returns the same bytes as address 0 (15-bit wrap).
- The image begins with `WTC` and contains `SAFwLTag`, `SACIdTag`, `SAHIDTag` and `SAVITECH`
  structures.
- Two independent full reads matched byte for byte.

Validate in steps: 8 bytes, then 16, then a boundary, and only then a full read.

## An open question: is it a general I2C bridge?

If byte 3 (`a0`) is an I2C device address rather than a constant, the Savitech may forward
reads and writes to any device on its I2C master bus. The ESP32's slave address 0x6B would then
be `0xD6`, which would give the computer a channel to the ESP32 while music plays (on the order
of 10 KB/s, estimated). Unknowns: whether the ESP32 sits on the same Savitech I2C master as the
EEPROM (the chip has more than one), and whether the command takes any address at all. A safe
first step is to read the updater's write path offline and see whether it treats `0xA0` as a
parameter. We never tested it live.
