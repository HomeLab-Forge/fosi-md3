"""Drive the MD3 over its USB control side channel (firmware 0.2.4 and later, Photo Sync mode).

Vendor requests on endpoint 0 work while the Mac has the NO NAME volume mounted, so nothing
here ejects or remounts the drive. Requests are defined in firmware/main/usb_ctl.h.

Usage:
  md3ctl.py info                      status JSON
  md3ctl.py key NAME [click|double|long|hold]   NAME: vol_up vol_down menu vista
  md3ctl.py shot [PATH]               screenshot as BMP (default captures/md3-<time>.bmp)
  md3ctl.py log                       the firmware's recent log text
  md3ctl.py offer                     return the volume to the Mac now and re-present it
  md3ctl.py reenumerate               USB re-enumeration from the Mac (works on any firmware)
  md3ctl.py rom                       reboot to ROM download mode (md3_deploy.py uses this)
"""

from __future__ import annotations

import datetime as dt
import json
import plistlib
import re
import struct
import subprocess
import sys
from pathlib import Path

import usb.core

PROJECT = Path(__file__).resolve().parent.parent
VID, PID = 0x303A, 0x4002
OUT, IN = 0x40, 0xC0
INFO, _TIME, KEY, SHOT, READ, LOG, OFFER, ROM, _SUN = range(1, 10)  # TIME and SUN retired with the clock
ROM_VALUE, ROM_INDEX = 0x524D, 0x3344
KEYS = {"vol_up": 0, "vol_down": 1, "menu": 2, "vista": 3}
PRESSES = {"click": 0, "double": 1, "long": 2, "hold": 3}
CHUNK = 32 * 1024
TIMEOUT_MS = 3000
MEDIA_NAME = "Fosi Audio MD3"
_volume_cache: dict[str, str] = {}


class NoDevice(RuntimeError):
    """The MD3's ESP32 is not on USB (unplugged, or in DAC mode)."""


class NoChannel(RuntimeError):
    """The device stalled a vendor request: firmware older than 0.2.4."""


def find() -> usb.core.Device:
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        raise NoDevice("MD3 Photo Sync (303a:4002) is not on USB")
    return dev


def volume() -> tuple[str, Path] | None:
    """(BSD device, mount point) of the MD3's mounted volume, found by its media name, not its label."""
    mounts = re.findall(r"^(/dev/disk\S+) on (.+) \(msdos,", subprocess.run(
        ["mount"], capture_output=True, text=True).stdout, re.M)
    for device, mount_point in mounts:
        if device not in _volume_cache:
            info = subprocess.run(["diskutil", "info", "-plist", device], capture_output=True).stdout
            _volume_cache[device] = plistlib.loads(info).get("MediaName", "") if info else ""
        if _volume_cache[device] == MEDIA_NAME:
            return device, Path(mount_point)
    return None


def _in(request: int, length: int, value: int = 0, index: int = 0, dev=None) -> bytes:
    dev = dev or find()
    try:
        return bytes(dev.ctrl_transfer(IN, request, value, index, length, timeout=TIMEOUT_MS))
    except usb.core.USBError as exc:
        if exc.errno == 32 or "Pipe" in str(exc):
            raise NoChannel(f"request {request:#x} stalled (firmware before 0.2.4?)") from exc
        raise


def _out(request: int, value: int = 0, index: int = 0, dev=None) -> None:
    dev = dev or find()
    try:
        dev.ctrl_transfer(OUT, request, value, index, None, timeout=TIMEOUT_MS)
    except usb.core.USBError as exc:
        if exc.errno == 32 or "Pipe" in str(exc):
            raise NoChannel(f"request {request:#x} stalled (bad argument or firmware before 0.2.4)") from exc
        raise


def info(dev=None) -> dict:
    return json.loads(_in(INFO, 2048, dev=dev).decode())


def key(name: str, press: str = "click", dev=None) -> None:
    _out(KEY, KEYS[name], PRESSES[press], dev=dev)


def shot(path: Path | None = None, dev=None) -> Path:
    dev = dev or find()
    size = struct.unpack("<I", _in(SHOT, 4, dev=dev))[0]
    if not size:
        raise RuntimeError("firmware could not capture the screen")
    data = bytearray()
    while len(data) < size:
        offset = len(data)
        part = _in(READ, min(CHUNK, size - offset), offset & 0xFFFF, offset >> 16, dev=dev)
        if not part:
            raise RuntimeError(f"short screenshot read at {offset}")
        data += part
    path = path or PROJECT / "captures" / f"md3-{dt.datetime.now():%Y%m%d-%H%M%S}.bmp"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(data))
    return path


def log(dev=None) -> str:
    return _in(LOG, 16383, dev=dev).decode(errors="replace")


def offer(dev=None) -> None:
    _out(OFFER, dev=dev)


def rom(dev=None) -> None:
    _out(ROM, ROM_VALUE, ROM_INDEX, dev=dev)


def reenumerate(dev=None) -> None:
    """Have macOS reset and re-enumerate the device; firmware returns the volume on SET_CONFIGURATION."""
    (dev or find()).reset()


def main(argv: list[str]) -> int:
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__.strip())
        return 0
    cmd, args = argv[0], argv[1:]
    try:
        if cmd == "info":
            print(json.dumps(info(), indent=2))
        elif cmd == "key":
            key(args[0], args[1] if len(args) > 1 else "click")
        elif cmd == "shot":
            print(shot(Path(args[0]) if args else None))
        elif cmd == "log":
            print(log())
        elif cmd == "offer":
            offer()
        elif cmd == "reenumerate":
            reenumerate()
        elif cmd == "rom":
            rom()
        else:
            print(f"unknown command {cmd!r}; see --help", file=sys.stderr)
            return 2
    except (NoDevice, NoChannel) as exc:
        print(exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
