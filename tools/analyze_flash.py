"""Map an ESP32-S3 flash dump: bootloader, partition table, app images and their metadata."""

from __future__ import annotations

import struct
import sys
from pathlib import Path

TYPES = {0x00: "app", 0x01: "data"}
APP_SUB = {0x00: "factory", 0x10: "ota_0", 0x11: "ota_1", 0x12: "ota_2", 0x20: "test"}
DATA_SUB = {0x00: "ota", 0x01: "phy", 0x02: "nvs", 0x03: "coredump", 0x04: "nvs_keys",
            0x05: "efuse", 0x80: "esphttpd", 0x81: "fat", 0x82: "spiffs", 0x83: "littlefs"}


def partitions(flash: bytes, offset: int = 0x8000) -> list[dict]:
    out = []
    for i in range(0, 0xC00, 32):
        e = flash[offset + i: offset + i + 32]
        magic, ptype, sub, off, size = struct.unpack_from("<HBBII", e)
        if magic != 0x50AA:
            break
        label = e[12:28].split(b"\x00")[0].decode(errors="replace")
        flags = struct.unpack_from("<I", e, 28)[0]
        subname = (APP_SUB if ptype == 0 else DATA_SUB).get(sub, f"{sub:#04x}")
        out.append({"label": label, "type": TYPES.get(ptype, hex(ptype)), "subtype": subname,
                    "offset": off, "size": size, "encrypted": bool(flags & 1)})
    return out


def app_desc(flash: bytes, off: int) -> dict | None:
    """esp_app_desc_t sits right after the first segment header (image hdr 24 + seg hdr 8)."""
    if flash[off] != 0xE9:
        return None
    d = flash[off + 32: off + 32 + 256]
    if struct.unpack_from("<I", d)[0] != 0xABCD5432:
        return {"note": "no esp_app_desc"}
    s = lambda a, b: d[a:b].split(b"\x00")[0].decode(errors="replace")
    return {"version": s(16, 48), "project": s(48, 80), "time": s(80, 96), "date": s(96, 112),
            "idf": s(112, 144), "elf_sha256": d[144:176].hex()}


def main() -> int:
    flash = Path(sys.argv[1]).read_bytes()
    print(f"image: {len(flash)} bytes")
    print(f"bootloader @0x0: magic {flash[0]:#04x}")
    for p in partitions(flash):
        used = flash[p["offset"]: p["offset"] + p["size"]]
        blank = used.count(0xFF) / max(1, len(used))
        print(f"{p['label']:<16} {p['type']:<5} {p['subtype']:<9} @{p['offset']:#09x} "
              f"size {p['size']:#09x} ({p['size'] / 1024 / 1024:6.2f} MB) "
              f"{'ENCRYPTED ' if p['encrypted'] else ''}erased {blank:5.1%}")
        if p["type"] == "app":
            print(f"    {app_desc(flash, p['offset'])}")
        if p["subtype"] == "ota":
            seq = [struct.unpack_from("<I", flash, p["offset"] + k)[0] for k in (0, 0x1000)]
            print(f"    otadata seq: {[hex(x) for x in seq]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
