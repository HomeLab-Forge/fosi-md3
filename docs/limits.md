# Hardware limits, and why we returned it

## The owner's verdict

After five days, for his use (listening from a phone): "it just doesn't work good enough as a
DAC." The unit went back to the store
on 2026-10-31 with its factory firmware restored byte for byte.

The owner used it to listen to music from a phone, not as a desk gadget for a computer. That
use case runs straight into the walls below.

## Hard limits (observed on one unit)

1. **No working radio** (this unit; cause inferred). The ESP32-S3 has Wi-Fi and Bluetooth LE, but on this unit:
   - a Wi-Fi scan from our firmware found 0 networks, in a home where other devices see Wi-Fi;
   - a Bluetooth LE beacon our firmware broadcast continuously (its name in every
     advertisement, no stack resets, about 4.7 hours of uptime) was never heard: not by a Mac
     scanning nearby, and not by a phone running nRF Connect held against the case.

   The antenna path is most likely not connected or not populated. So the device cannot get
   the time, notifications or now-playing data from a phone.
2. **One port, two devices.** The USB data lines go to the audio bridge or to the ESP32, never
   both. In Audio mode the computer can't reach the screen controller at all; switching modes
   looks to the computer like unplugging one device and plugging in another.
3. **The screen knows almost nothing about the music.** The ESP32 never sees audio samples. It
   learns the sample rate, DSD, and the host's volume and mute from the audio bridge, and it can
   read the DAC's status registers. No spectrum, no level meter, no track title.
4. **Nothing flows back to the phone or computer in Audio mode.** The ESP32's link from the
   audio bridge is one-way, and the buttons generate no HID traffic.
5. **No sound of its own.** No alert tones, no voice: the ESP32 can only shape what plays
   (volume, mute, filter, gain).
6. **Alive only while plugged in.** Bus-powered, no battery, no real-time clock. The time is
   lost at every unplug.
7. **No sensors.** No microphone, motion, light or touch sensing. The stock firmware rotates
   the screen when you double-press Vista, which fits.

## Stock software problems we found

From static analysis of the stock firmware ([`stock-firmware.md`](stock-firmware.md)):

- New pictures appear only after a power cycle: the folders are scanned at boot only.
- PNG is in the accepted file list but never displays ("open picture error").
- A GIF is read whole into PSRAM with no size check.
- The "VBUS overflow!" warning only draws a label. Despite the log text, the firmware does not
  switch anything off.
- On first boot the volume defaults to 100 of 100 (no digital attenuation) with gain High.
  After a factory reset it is 26.
- The settings record is written with no checksum.
- The boot log prints a "key verified" message unconditionally; no key or MAC check exists.
- No public firmware, update tool or update procedure exists for owners. The device has no OTA
  slots, so updates presumably go through the ESP32's USB boot window (inferred).

## What was still possible, in software only

For the record, the ceiling we mapped before the decision:

- **Without a host:** a listening trace from the DAC's automute (silence) flags, a daily volume
  dose and cap, your own pictures and decks, offline text.
- **With a computer in Photo Sync mode:** anything on the screen, driven over USB (no audio in
  that mode).
- **With a computer in Audio mode (untested):** a slow data channel through the left and right
  host volume values, which the stock firmware already receives and which ours can ignore for
  loudness (order of 100 bits/s, estimated). On an iPhone this would need iOS to send volume as
  a USB command, which no public source confirms.
- **Through the Savitech, if it bridges I2C (untested):** see [`savitech.md`](savitech.md).
