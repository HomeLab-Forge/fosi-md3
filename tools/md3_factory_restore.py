"""Return the MD3's flash to the factory dump, byte for byte, and leave the chip in download mode.

Writes everything from 0x9000 to the end of the 32 MB flash (nvs, phy_init, the factory app,
the Photo Sync volume and the settings record) from a full dump taken before any custom
firmware. The bootloader (0x0) and partition table (0x8000) are never written: the tool reads
them first and refuses to continue unless they already match the dump. It finishes by checking
the whole flash against the dump on the chip (MD5), then reading it all back for a SHA-256.

The chip stays in ROM download mode afterwards. The factory settings record boots the device
in DAC mode, which would appear on the host as an audio output, so the tool does not reset it.
Unplug the MD3 when it finishes; the next power-up runs the factory firmware.

Usage: md3_factory_restore.py DUMP   (a full 32 MB read of your own unit, taken before any
custom firmware: `esptool read-flash 0 0x2000000 DUMP` with the chip in download mode)
"""

from __future__ import annotations

import fcntl
import hashlib
import os
import subprocess
import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import md3_deploy  # noqa: E402

PROJECT = Path(__file__).resolve().parent.parent
FLASH_BYTES = 32 * 1024 * 1024
KEEP = 0x9000  # bootloader and partition table: compared, never written
log = md3_deploy.log


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    dump_path = Path(sys.argv[1])
    dump = dump_path.read_bytes()
    if len(dump) != FLASH_BYTES:
        raise SystemExit(f"{dump_path} is {len(dump)} bytes, expected {FLASH_BYTES}")
    dump_sha = hashlib.sha256(dump).hexdigest()
    log(f"dump {dump_path.name}: sha256 {dump_sha}")

    md3_deploy.LOCK_PATH.parent.mkdir(parents=True, exist_ok=True)
    lock = open(md3_deploy.LOCK_PATH, "a")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        raise SystemExit(f"another tool holds {md3_deploy.LOCK_PATH}")
    lock.truncate(0)
    lock.write(f"{os.getpid()} factory-restore\n")
    lock.flush()

    out_dir = PROJECT / "backup" / f"factory-restore-{datetime.now():%Y-%m-%d-%H%M%S}"
    out_dir.mkdir(parents=True)
    port = md3_deploy.rom_port() or md3_deploy.catch(30, out_dir)
    flags = ("--port", port, "--before", "no-reset", "--after", "no-reset")
    log(f"ROM download mode on {port}")

    head = out_dir / "head-0x0-0x9000.bin"
    md3_deploy.esptool(*flags, "read-flash", "0x0", hex(KEEP), str(head))
    if head.read_bytes() != dump[:KEEP]:
        log("bootloader or partition table differs from the dump: stopping without writing")
        return 2
    log("bootloader and partition table already match the dump")

    body = out_dir / "body-0x9000.bin"
    body.write_bytes(dump[KEEP:])
    log(f"writing {len(dump) - KEEP} bytes at {KEEP:#x}")
    md3_deploy.esptool(*flags, "write-flash", hex(KEEP), str(body))
    body.unlink()

    log("verifying the whole flash against the dump on the chip (MD5)")
    md3_deploy.esptool(*flags, "verify-flash", "0x0", str(dump_path))
    readback = out_dir / "readback-full.bin"
    log("reading the whole flash back")
    md3_deploy.esptool(*flags, "read-flash", "0x0", hex(FLASH_BYTES), str(readback))
    read_sha = hashlib.sha256(readback.read_bytes()).hexdigest()
    (out_dir / "result.txt").write_text(f"dump     {dump_sha}\nreadback {read_sha}\n")
    readback.unlink()
    if read_sha != dump_sha:
        log(f"READBACK MISMATCH: {read_sha}")
        return 3
    log(f"flash is byte-identical to the factory dump (sha256 {read_sha[:16]})")
    log("chip held in download mode; unplug the MD3 to finish")
    return 0


if __name__ == "__main__":
    sys.exit(main())
