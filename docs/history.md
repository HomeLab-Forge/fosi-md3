# How it went: five days, 2026-10-26 to 2026-10-31

The owner asked for "complete control of the entire thing ... total ownership of the software".
The work was done by AI coding agents on the owner's Mac: Kimi K3.1 Pro in OpenCode, and Claude
in Claude Code. The owner supplied intent, rulings and, until the firmware could reflash
itself, his hands on the cable. Times are US Pacific.

## Timeline

| When | What happened |
|------|---------------|
| 10-26 afternoon | USB strings show two chips behind one port: a Savitech audio bridge and an Espressif part running the ESP-IDF TinyUSB MSC example. No public firmware, updater or ROM-entry procedure exists. |
| 10-26 afternoon | The Savitech EEPROM read command is recovered from FiiO's Q11 updater (a different product, same chip family), validated in small steps, and the 32 KiB EEPROM backed up twice with matching hashes. The photo volume is imaged the same way. |
| 10-26 16:02 | The ESP32-S3's USB boot window is found and caught. Secure boot and flash encryption are off. The full 32 MB flash is dumped. |
| 10-26 evening | The stock app is disassembled offline: pin map, DAC driver, the Savitech link protocol, the settings record, menus and keys. |
| 10-26 to 10-27 | A complete replacement firmware is written on disk, including a Wi-Fi HTTP API. Five review rounds before anything touches the device. |
| 10-27 20:47 | Pivot: the owner had no use for Wi-Fi. Photo Sync, a stock feature he used, becomes the priority. The `esp_tinyusb` storage helper's format-on-failed-mount path is removed from the link. |
| 10-28 17:42 | Restoring the stock app is proven before any custom build is trusted. |
| 10-28 22:08 | The custom firmware boots: its own UI, Photo Sync working. The screen is upside down and mirrored, and the album shows "No pictures". |
| 10-29 22:27 | Upright on the third orientation attempt; Fosi's GIF87a logo plays. |
| 10-29 22:52 | First audio through the custom firmware: PCM 48k, volume keys correct. "It sounds good." |
| 10-29 23:14 | First flash with no human: the firmware reboots itself into download mode on request. 34 s end to end. |
| 10-29 night | Radio probes: a Wi-Fi scan finds 0 networks and a BLE beacon goes unheard. Screenshots and key injection over the cable. |
| 10-30 00:11 | The owner's 24-hour day-wheel appears on the device, about 100 minutes after he described it. |
| 10-30 18:17 | macOS ejects the drive at wake and firmware 0.2.3 keeps it dark for hours. |
| 10-31 00:12 | Firmware 0.2.4: a USB control channel that works with the drive mounted, the drive returns by itself after any eject, 20 s hands-free deploys. |
| 10-31 17:30 | A phone held against the case hears no beacon: no radio, for good. |
| 10-31 evening | The owner decides to return it. At 18:37 the whole flash is restored and read back byte-identical to the 10-26 dump. |

Every write to the ESP32 went to 0x10000 (until the final full restore), and every one was read
back and compared. The bootloader, partition table and eFuses were never touched.

## Diagnoses worth keeping

- **Black album.** LVGL 9.2's GIF decoder accepts only GIF89a, Fosi's album art is GIF87a, and
  the load fails without an error. Found by reading LVGL's `gifdec.c`. The format had been
  noted in the project's own findings before the firmware was written.
- **Orientation.** The owner's first description, "upside down and mirrored", was exact: a
  vertical flip, `mirror(false, true)`. Reading his photos as a rotation, then as a horizontal
  flip, cost three extra flash cycles. Each cost him a replug.
- **The dark drive.** After an eject, firmware 0.2.3 mounted the FAT for itself to look for
  request files, then refused the host until a USB re-enumeration. Every agent tool
  reconnected after its own request, so the bug hid until macOS ejected the drive at wake.
  An IOKit re-enumeration from the Mac brought it back in 2.2 s, and standard control
  transfers kept working with the volume mounted. That became the control channel.
- **Slow ROM entry.** After the firmware handed the USB PHY to the ROM, the Mac kept talking
  to the old device for minutes. Dropping the pull-up for 100 ms before the handover made ROM
  mode appear 1 s after the request.

## Lessons for agent-driven hardware work

1. **Prove the way back first.** Dump everything, read it twice, and restore stock once before
   trusting anything new.
2. **Remove dangerous paths, don't just flag them off.** The formatting helper left the link,
   and a test fails if `f_mkfs` is ever called.
3. **Give the agent eyes and hands over the cable early.** Most of the elapsed time was waiting
   for a human to replug. Once the firmware could reflash itself, take screenshots and press
   its own buttons, a build-flash-verify loop took 20 s.
4. **Settle a geometric transform with an explicit check.** Render the candidates side by side
   and pick the one that reads.
5. **Re-read your own notes before writing code against a format.**
6. **Ask what the owner will actually use it for, early.** The best engineering here served a
   desk instrument; the owner wanted a DAC for his phone.
