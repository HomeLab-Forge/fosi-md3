"""Dump and parse the MD3's USB configuration descriptor without opening the device.

libusb on macOS reads the configuration descriptor from the IORegistry cache, so this
issues no control transfers and claims no interfaces.
"""

from __future__ import annotations

import os
import sys

import usb.backend.libusb1
import usb.core

VID, PID = 0x262A, 0x1A86

DESC_TYPES = {
    0x02: "CONFIGURATION",
    0x04: "INTERFACE",
    0x05: "ENDPOINT",
    0x0B: "IAD",
    0x21: "HID",
    0x24: "CS_INTERFACE",
    0x25: "CS_ENDPOINT",
}

AC_SUBTYPES = {
    0x01: "HEADER",
    0x02: "INPUT_TERMINAL",
    0x03: "OUTPUT_TERMINAL",
    0x04: "MIXER_UNIT",
    0x05: "SELECTOR_UNIT",
    0x06: "FEATURE_UNIT",
    0x07: "EFFECT_UNIT",
    0x08: "PROCESSING_UNIT",
    0x09: "EXTENSION_UNIT",
    0x0A: "CLOCK_SOURCE",
    0x0B: "CLOCK_SELECTOR",
    0x0C: "CLOCK_MULTIPLIER",
}

AS_SUBTYPES = {0x01: "AS_GENERAL", 0x02: "FORMAT_TYPE"}

FEATURE_CONTROLS = [
    "mute", "volume", "bass", "mid", "treble", "graphic_eq", "agc", "delay",
    "bass_boost", "loudness", "input_gain", "input_gain_pad", "phase_inverter",
    "underflow", "overflow",
]

TERMINAL_TYPES = {
    0x0101: "USB streaming",
    0x0301: "speaker",
    0x0302: "headphones",
    0x0603: "line connector",
    0x0605: "S/PDIF",
}


def control_bits(bitmap: int, names: list[str]) -> list[str]:
    """Decode a UAC2 2-bits-per-control bitmap into 'name:r' / 'name:rw' labels."""
    out = []
    for i, name in enumerate(names):
        v = (bitmap >> (2 * i)) & 0b11
        if v == 0b01:
            out.append(f"{name}:r")
        elif v == 0b11:
            out.append(f"{name}:rw")
    return out


def parse(raw: bytes) -> None:
    """Walk the raw configuration descriptor and print each descriptor decoded."""
    i = 0
    iface_class = None
    while i < len(raw):
        length, dtype = raw[i], raw[i + 1]
        d = raw[i:i + length]
        name = DESC_TYPES.get(dtype, f"0x{dtype:02x}")
        line = f"{i:4d} {name:<14} {d.hex(' ')}"
        if dtype == 0x04:
            iface_class = (d[5], d[6])
            line += (f"\n       iface={d[2]} alt={d[3]} eps={d[4]} class={d[5]:#04x}"
                     f" sub={d[6]:#04x} proto={d[7]:#04x} iInterface={d[8]}")
        elif dtype == 0x05:
            addr, attrs = d[2], d[3]
            kind = ["control", "iso", "bulk", "interrupt"][attrs & 3]
            sync = ["none", "async", "adaptive", "sync"][(attrs >> 2) & 3]
            usage = ["data", "feedback", "implicit fb", "?"][(attrs >> 4) & 3]
            mps = d[4] | (d[5] << 8)
            line += (f"\n       ep={addr:#04x} ({'IN' if addr & 0x80 else 'OUT'}) {kind}"
                     f" sync={sync} usage={usage} maxpkt={mps} interval={d[6]}")
        elif dtype == 0x21:
            rlen = d[7] | (d[8] << 8)
            line += f"\n       HID bcd={d[3]:02x}{d[2]:02x} report_desc_len={rlen}"
        elif dtype == 0x24 and iface_class == (1, 1):
            sub = d[2]
            line += f"\n       AC {AC_SUBTYPES.get(sub, hex(sub))}"
            if sub == 0x0A:
                attrs = d[4]
                ctype = ["external", "internal fixed", "internal variable", "internal prog"][attrs & 3]
                line += (f" id={d[3]} type={ctype} sof_sync={(attrs >> 2) & 1}"
                         f" controls={control_bits(d[5], ['freq', 'validity'])}")
            elif sub == 0x0B:
                n = d[4]
                line += f" id={d[3]} sources={list(d[5:5 + n])}"
            elif sub == 0x02:
                tt = d[4] | (d[5] << 8)
                line += (f" id={d[3]} type={TERMINAL_TYPES.get(tt, hex(tt))} clock={d[7]}"
                         f" channels={d[8]}")
            elif sub == 0x03:
                tt = d[4] | (d[5] << 8)
                line += (f" id={d[3]} type={TERMINAL_TYPES.get(tt, hex(tt))} source={d[7]}"
                         f" clock={d[8]}")
            elif sub == 0x06:
                uid, src = d[3], d[4]
                n = (length - 6) // 4
                line += f" id={uid} source={src}"
                for ch in range(n):
                    bm = int.from_bytes(d[5 + 4 * ch:9 + 4 * ch], "little")
                    label = "master" if ch == 0 else f"ch{ch}"
                    line += f"\n         {label}: {control_bits(bm, FEATURE_CONTROLS)}"
            elif sub in (0x08, 0x09):
                line += f" id={d[3]} (vendor/processing unit: payload {d[4:].hex(' ')})"
        elif dtype == 0x24 and iface_class == (1, 2):
            sub = d[2]
            line += f"\n       AS {AS_SUBTYPES.get(sub, hex(sub))}"
            if sub == 0x02 and len(d) >= 6:
                line += f" type={d[3]} subslot={d[4]} bits={d[5]}"
            elif sub == 0x01:
                fmts = int.from_bytes(d[6:10], "little")
                line += (f" terminal={d[3]} formats={fmts:#010x}"
                         f" (bit0=PCM bit31=RAW/DSD) channels={d[10]}")
        print(line)
        i += length


def main() -> int:
    brew = "/opt/homebrew/lib/libusb-1.0.dylib"
    backend = usb.backend.libusb1.get_backend(find_library=lambda _: brew) if os.path.exists(brew) else None
    dev = usb.core.find(idVendor=VID, idProduct=PID, backend=backend)
    if dev is None:
        print("MD3 not found", file=sys.stderr)
        return 1
    print(
        f"device: usb={dev.bcdUSB:#06x} class={dev.bDeviceClass:#04x}/{dev.bDeviceSubClass:#04x}"
        f"/{dev.bDeviceProtocol:#04x} ep0={dev.bMaxPacketSize0} bcdDevice={dev.bcdDevice:#06x}"
    )
    parse(rebuild_config(dev[0]))
    return 0


def rebuild_config(cfg: usb.core.Configuration) -> bytes:
    """Reassemble raw descriptor bytes from libusb's parsed tree (it keeps class-specific extras)."""
    out = bytearray([9, 0x02, cfg.wTotalLength & 0xFF, cfg.wTotalLength >> 8,
                     cfg.bNumInterfaces, cfg.bConfigurationValue, cfg.iConfiguration,
                     cfg.bmAttributes, cfg.bMaxPower])
    out += bytes(cfg.extra_descriptors)
    for intf in cfg:
        out += bytes([9, 0x04, intf.bInterfaceNumber, intf.bAlternateSetting,
                      intf.bNumEndpoints, intf.bInterfaceClass, intf.bInterfaceSubClass,
                      intf.bInterfaceProtocol, intf.iInterface])
        out += bytes(intf.extra_descriptors)
        for ep in intf:
            out += bytes([ep.bLength, 0x05, ep.bEndpointAddress, ep.bmAttributes,
                          ep.wMaxPacketSize & 0xFF, ep.wMaxPacketSize >> 8, ep.bInterval])
            if ep.bLength == 9:
                out += bytes([ep.bRefresh, ep.bSynchAddress])
            out += bytes(ep.extra_descriptors)
    if len(out) != cfg.wTotalLength:
        print(f"note: rebuilt {len(out)} bytes vs wTotalLength {cfg.wTotalLength}")
    return bytes(out)


if __name__ == "__main__":
    sys.exit(main())
