"""Deploy an app image to the MD3 without touching it: request ROM mode, write 0x10000 only,
verify the readback, and boot the new image.

Firmware 0.2.4 and later takes the ROM request over the USB control channel (md3ctl.py rom),
which needs no volume. Firmware 0.1.5 and later reboots into ROM download mode when the host
writes /MD3ROM.TXT (starting with "md3-enter-rom") to the Photo Sync volume and ejects it; the
firmware deletes the file first, so the request fires once. If the device is on USB but its
volume is dark, the tool re-enumerates it to bring the volume back. Only when the device is
not on USB at all (Audio mode, stock firmware off the bus) does it ask for a replug.

Only the application partition at 0x10000 is ever written. A failed readback stops with the
chip held in download mode and prints the restore command. One deploy at a time: the device
is shared, so the tool holds an exclusive lock for its whole run.

Usage: md3_deploy.py [IMAGE] [--wait SECONDS]
"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import os
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

import md3ctl

PROJECT = Path(__file__).resolve().parent.parent
TOOLS = PROJECT / "tools"
DEFAULT_IMAGE = PROJECT / "firmware" / "build" / "md3fw.bin"
FACTORY_APP = PROJECT / "backup" / "factory-app.bin"  # your own unit's app partition, read before flashing
LOCK_PATH = Path.home() / ".md3" / "device.lock"
VOLUME = Path("/Volumes/NO NAME")
REQUEST_NAME = "MD3ROM.TXT"
REQUEST_MARKER = "md3-enter-rom"
APP_OFFSET = 0x10000
APP_PARTITION_BYTES = 9 * 1024 * 1024


def log(msg: str) -> None:
    print(f"{datetime.now():%H:%M:%S} {msg}", flush=True)


def esptool(*args: str) -> None:
    subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s3", *args], check=True,
                   stdout=subprocess.DEVNULL)


def rom_port() -> str | None:
    """The ESP32-S3's USB-Serial-JTAG port (303a:1001), or None. Other serial devices are ignored."""
    from serial.tools import list_ports  # pyserial, installed with esptool
    ports = sorted(p.device for p in list_ports.comports()
                   if p.vid == 0x303A and p.pid == 0x1001 and p.device.startswith("/dev/cu."))
    return ports[0] if ports else None


def volume_device() -> str | None:
    """The BSD device of the MD3's mounted Photo Sync volume (matched by media name), or None."""
    global VOLUME
    found = md3ctl.volume()
    if not found:
        return None
    VOLUME = found[1]
    return found[0]


def request_rom(device: str) -> None:
    """Write the ROM request, drop macOS's AppleDouble shadow, and eject."""
    request = VOLUME / REQUEST_NAME
    with open(request, "w") as f:
        f.write(REQUEST_MARKER + "\n")
        f.flush()
        os.fsync(f.fileno())
    (VOLUME / f"._{REQUEST_NAME}").unlink(missing_ok=True)
    subprocess.run(["sync"], check=True)
    subprocess.run(["diskutil", "eject", device], check=True, stdout=subprocess.DEVNULL)


def request_rom_over_control() -> bool:
    """Ask firmware 0.2.4+ for ROM mode on the USB control channel; False if it cannot."""
    try:
        md3ctl.info()  # NoChannel on older firmware, before anything is ejected
        if mounted := md3ctl.volume():
            # Never reboot under a mounted volume: flush and eject first. The channel survives it.
            subprocess.run(["sync"], check=True)
            subprocess.run(["diskutil", "eject", mounted[0]], check=True, stdout=subprocess.DEVNULL)
        md3ctl.rom()
        return True
    except (md3ctl.NoDevice, md3ctl.NoChannel):
        return False


def recover_volume() -> str | None:
    """Re-enumerate a device that is on USB with its volume dark; return the volume's device."""
    try:
        md3ctl.reenumerate()
    except md3ctl.NoDevice:
        return None
    log("device on USB without its volume; re-enumerated it")
    for _ in range(15):
        time.sleep(1)
        if device := volume_device():
            return device
    return None


def catch(wait: float, out_dir: Path) -> str:
    """Bring the chip to ROM download mode and hold it; return its port."""
    listener = subprocess.Popen(
        [sys.executable, str(TOOLS / "catch_rom.py"), "--wait", str(wait), "--hold"],
        stdout=open(out_dir / "catch.log", "w"), stderr=subprocess.STDOUT)
    time.sleep(1)
    if request_rom_over_control():
        log("requested ROM mode over the USB control channel")
    elif device := volume_device() or recover_volume():
        log("requesting ROM mode over Photo Sync")
        request_rom(device)
    else:
        log("MD3 not on USB: unplug and replug the MD3's bottom port, buttons released")
    if listener.wait() != 0:
        raise SystemExit(f"ROM window not caught; see {out_dir / 'catch.log'}")
    port = rom_port()
    if not port:
        raise SystemExit("caught, but no /dev/cu.usbmodem* port remains")
    return port


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image", nargs="?", type=Path, default=DEFAULT_IMAGE)
    ap.add_argument("--wait", type=float, default=120,
                    help="seconds to wait for ROM mode (default 120; give longer for a replug)")
    args = ap.parse_args()

    image = args.image.resolve()
    data = image.read_bytes()
    if not data or data[0] != 0xE9 or len(data) > APP_PARTITION_BYTES:
        raise SystemExit(f"{image} is not an ESP app image that fits the 9 MiB app partition")
    digest = hashlib.sha256(data).hexdigest()

    LOCK_PATH.parent.mkdir(parents=True, exist_ok=True)
    lock = open(LOCK_PATH, "a")
    for _ in range(30):  # another tool may hold the lock briefly; a deploy holds it for minutes
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            break
        except BlockingIOError:
            time.sleep(0.5)
    else:
        raise SystemExit(f"another tool holds {LOCK_PATH} (a deploy?)")
    lock.truncate(0)
    lock.write(f"{os.getpid()} {image}\n")
    lock.flush()

    out_dir = PROJECT / "backup" / f"deploy-{datetime.now():%Y-%m-%d-%H%M%S}"
    out_dir.mkdir(parents=True)
    (out_dir / "image.sha256").write_text(f"{digest}  {image}\n")
    log(f"image {image.name}: {len(data)} bytes, sha256 {digest[:16]}")

    port = rom_port() or catch(args.wait, out_dir)
    log(f"ROM download mode on {port}; writing {len(data)} bytes at {APP_OFFSET:#x}")
    flags = ("--port", port, "--before", "no-reset", "--after", "no-reset")
    esptool(*flags, "write-flash", hex(APP_OFFSET), str(image))
    readback = out_dir / "readback.bin"
    esptool(*flags, "read-flash", hex(APP_OFFSET), str(len(data)), str(readback))
    if readback.read_bytes() != data:
        log("READBACK MISMATCH: the chip is held in download mode. Restore stock with:")
        log(f"  esptool --port {port} write-flash {APP_OFFSET:#x} {FACTORY_APP}")
        return 2
    log("readback identical")

    subprocess.run([sys.executable, str(TOOLS / "catch_rom.py"), "--port", port], check=True,
                   stdout=subprocess.DEVNULL)
    log("watchdog reset sent; waiting for the app")
    for _ in range(30):
        if volume_device():
            (VOLUME / f"._{REQUEST_NAME}").unlink(missing_ok=True)
            log(f"booted: Photo Sync volume is back at {VOLUME}")
            return 0
        time.sleep(1)
    log("booted, but no Photo Sync volume within 30 s (Audio mode, or the app did not start)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
