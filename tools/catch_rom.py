"""Catch the ESP32-S3's boot-time USB-Serial-JTAG window and hold the chip in ROM download mode.

At every power-up the MD3 exposes 303a:1001 ("USB JTAG_serial debug unit") for about one
second before its firmware switches the port to the Savitech audio chip. Opening that port
and issuing esptool's USB-JTAG reset puts the S3 into ROM download mode; the application
never starts, so the port stays up. Everything here is read-only: the stub loader runs from
RAM and nothing is written to flash or eFuses.

Usage: run it, then unplug and replug the MD3 (bottom port) without touching any button.
Ends with a watchdog reset so the MD3 boots normally again, unless --hold is given.
"""

from __future__ import annotations

import argparse
import contextlib
import glob
import hashlib
import io
import os
import sys
import time
from datetime import datetime
from pathlib import Path

from esptool.cmds import (attach_flash, detect_chip, flash_id, get_security_info, read_flash,
                          read_mac, reset_chip, run_stub)

PROJECT = Path(__file__).resolve().parent.parent


class Tee(io.TextIOBase):
    def __init__(self, *streams: io.TextIOBase) -> None:
        self.streams = streams

    def write(self, s: str) -> int:
        for st in self.streams:
            st.write(s)
            st.flush()
        return len(s)


def log(msg: str) -> None:
    print(f"{datetime.now().strftime('%H:%M:%S.%f')[:-3]} {msg}", flush=True)


# The boot window's USB id. On Linux the port is a /dev/ttyACM* node, and other
# Espressif devices can sit on the same host (a Zigbee/Thread stick is 303a:831a),
# so a fresh node only counts when its USB device is this one.
BOOT_ID = ("303a", "1001")


def usb_id(port: str) -> tuple[str, str] | None:
    """The (idVendor, idProduct) of the USB device behind a Linux tty node."""
    dev = Path("/sys/class/tty") / Path(port).name / "device"
    with contextlib.suppress(OSError):
        usb = dev.resolve().parent  # the interface's parent is the USB device
        return ((usb / "idVendor").read_text().strip(), (usb / "idProduct").read_text().strip())
    return None


def candidate_ports() -> set[str]:
    if sys.platform == "darwin":
        return set(glob.glob("/dev/cu.usbmodem*"))
    return {p for p in glob.glob("/dev/ttyACM*") if usb_id(p) == BOOT_ID}


def wait_for_port(seconds: float) -> str | None:
    existing = candidate_ports()
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        # On Linux the node appears a few ms before udev gives it its group; opening it
        # then fails with EACCES, so a fresh node only counts once it is writable.
        fresh = {p for p in candidate_ports() - existing if os.access(p, os.R_OK | os.W_OK)}
        if fresh:
            return sorted(fresh)[0]
        time.sleep(0.005)
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--wait", type=float, default=600, help="seconds to wait for the window")
    ap.add_argument("--dump", action="store_true", help="also read the whole flash")
    ap.add_argument("--port", help="skip waiting; chip already in download mode on this port")
    ap.add_argument("--hold", action="store_true",
                    help="leave the chip in download mode (default: watchdog-reset back to normal boot)")
    args = ap.parse_args()

    outdir = PROJECT / "backup" / f"flash-{datetime.now().strftime('%Y-%m-%d-%H%M%S')}"
    outdir.mkdir(parents=True, exist_ok=True)
    logfile = open(outdir / "esptool.log", "a")
    sys.stdout = Tee(sys.__stdout__, logfile)  # type: ignore[assignment]

    if args.port:
        port, mode = args.port, "no-reset"
    else:
        log("waiting for the boot window; unplug and replug the MD3 now (no buttons)")
        port = wait_for_port(args.wait)
        if not port:
            log("window never appeared")
            return 1
        log(f"caught {port}")
        mode = "default-reset"  # on a USB-Serial-JTAG port this is the USB-JTAG reset sequence

    try:
        esp = detect_chip(port, baud=115200, connect_mode=mode, connect_attempts=3)
    except Exception as e:
        log(f"first contact failed ({e!r}); the firmware likely switched the port away first")
        return 2
    log(f"connected: {esp.get_chip_description()}  features={esp.get_chip_features()}")
    with contextlib.suppress(Exception):
        get_security_info(esp)
    read_mac(esp)
    esp = run_stub(esp)
    attach_flash(esp)
    flash_id(esp)
    fid = esp.flash_id()
    size = 1 << ((fid >> 16) & 0xFF)
    log(f"flash JEDEC id {fid:#08x}, size {size // (1024 * 1024)} MB")

    if args.dump:
        image = outdir / "flash-full.bin"
        t0 = time.monotonic()
        read_flash(esp, 0, size, str(image))
        digest = hashlib.sha256(image.read_bytes()).hexdigest()
        (outdir / "flash-full.sha256").write_text(f"{digest}  flash-full.bin\n")
        log(f"dumped {image.stat().st_size} bytes in {time.monotonic() - t0:.0f}s, sha256 {digest}")

    log(f"artifacts: {outdir}")
    if args.hold:
        esp._port.close()
        log("chip held in download mode; `catch_rom.py --port <p>` reuses it, a replug boots normally")
    else:
        # A plain RTS hard reset re-enters download mode over USB-Serial-JTAG; the watchdog
        # reset boots the application (verified 2026-10-26).
        reset_chip(esp, "watchdog-reset")
        log("watchdog reset sent; the MD3 should be back in audio mode within ~2 s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
