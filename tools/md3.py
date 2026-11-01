"""md3: own the Fosi Audio MD3's screen content from the command line.

The MD3 is two USB devices behind one port:
  audio mode       Savitech USB audio bridge   262a:1a86  (UAC2 + vendor HID)
  photo sync mode  Espressif ESP32 (TinyUSB)   303a:4002  (mass storage, FAT16 "NO NAME")
Switching modes is done on the device: Menu > USB Mode.

Everything the screen shows outside the volume overlay comes from GIF/JPG files in six
folders on the ESP32's flash: Photo1-3 (album) and Game1-3 (each game picks one file at
random, plays it once and holds on its last frame).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import plistlib
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

from PIL import Image, ImageDraw, ImageOps, ImageSequence

PROJECT = Path(__file__).resolve().parent.parent
BACKUPS = PROJECT / "backup"
FOLDERS = ("Photo1", "Photo2", "Photo3", "Game1", "Game2", "Game3")
SIZE = 240
SIZE_WARN = 2 * 1024 * 1024  # Fosi's own tool warns above 2 MB

MODES = {
    (0x262A, 0x1A86): "audio",
    (0x303A, 0x4002): "photo-sync",
}


# ---------------------------------------------------------------- device state

def usb_md3() -> list[dict]:
    """Every USB device that is (or might be) the MD3: its known IDs plus any Espressif device."""
    raw = subprocess.run(["ioreg", "-p", "IOUSB", "-l", "-a"], capture_output=True).stdout
    found: list[dict] = []

    def walk(node: dict) -> None:
        vid, pid = node.get("idVendor"), node.get("idProduct")
        if vid is not None and ((vid, pid) in MODES or vid == 0x303A):
            found.append({
                "vid": vid, "pid": pid,
                "mode": MODES.get((vid, pid), "espressif-unknown"),
                "name": node.get("USB Product Name", ""),
                "serial": node.get("USB Serial Number", ""),
                "location": f"{node.get('locationID', 0):#010x}",
            })
        for child in node.get("IORegistryEntryChildren", []):
            walk(child)

    walk(plistlib.loads(raw))
    return found


def mount_point() -> Path | None:
    """The mounted MD3 volume, found by its media name rather than its volume label."""
    raw = subprocess.run(["diskutil", "list", "-plist", "external", "physical"],
                         capture_output=True).stdout
    try:
        disks = plistlib.loads(raw).get("AllDisksAndPartitions", [])
    except Exception:
        return None
    for d in disks:
        info = plistlib.loads(subprocess.run(["diskutil", "info", "-plist", d["DeviceIdentifier"]],
                                             capture_output=True).stdout)
        if "MD3" in info.get("MediaName", "") and info.get("MountPoint"):
            return Path(info["MountPoint"])
    return None


def require_mount() -> Path:
    mp = mount_point()
    if mp is None:
        devs = usb_md3()
        mode = devs[0]["mode"] if devs else "not connected"
        if mode == "photo-sync":
            sys.exit("MD3 is in Photo Sync but its volume is ejected; unplug and replug it to remount.")
        sys.exit(f"MD3 volume not mounted (device: {mode}). On the MD3: Menu > USB Mode > "
                 f"Photo Sync Mode, then retry.")
    return mp


# ---------------------------------------------------------------- media

def gif_summary(path: Path) -> dict:
    try:
        im = Image.open(path)
        frames = getattr(im, "n_frames", 1)
        last = 0
        if frames > 1:
            im.seek(frames - 1)
            last = im.info.get("duration", 0)
        return {"w": im.width, "h": im.height, "frames": frames, "hold_last_ms": last,
                "format": im.format}
    except Exception as e:
        return {"error": str(e)}


def circle_mask() -> Image.Image:
    m = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(m).ellipse([0, 0, SIZE - 1, SIZE - 1], fill=255)
    return m


def convert(src: Path, dst: Path, fit: str = "cover", fps: float | None = None,
            hold_last_ms: int | None = None, colors: int = 128, bg: str = "#000000") -> Path:
    """Convert any still or animated image into a 240x240 GIF the MD3 plays.

    fit: "cover" fills the circle (crops), "contain" letterboxes inside it.
    hold_last_ms: make the final frame linger (how the stock games show a result).
    """
    im = Image.open(src)
    frames, durations = [], []
    for f in ImageSequence.Iterator(im):
        rgb = f.convert("RGBA")
        if fit == "cover":
            out = ImageOps.fit(rgb, (SIZE, SIZE), Image.Resampling.LANCZOS)
        else:
            out = Image.new("RGBA", (SIZE, SIZE), bg)
            inner = int(SIZE * 0.72)  # largest square inside the circle is 240/sqrt(2) ~ 170
            rgb.thumbnail((inner, inner), Image.Resampling.LANCZOS)
            out.paste(rgb, ((SIZE - rgb.width) // 2, (SIZE - rgb.height) // 2), rgb)
        flat = Image.new("RGB", (SIZE, SIZE), bg)
        flat.paste(out, (0, 0), out)
        frames.append(flat)
        durations.append(int(1000 / fps) if fps else f.info.get("duration", im.info.get("duration", 100)) or 100)
    if hold_last_ms is not None:
        durations[-1] = hold_last_ms
    q = [f.quantize(colors=colors, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
         for f in frames]
    dst.parent.mkdir(parents=True, exist_ok=True)
    if len(q) == 1:
        q[0].save(dst, format="GIF")
    else:
        q[0].save(dst, format="GIF", save_all=True, append_images=q[1:], duration=durations,
                  loop=0, disposal=1, optimize=False)
    return dst


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def copy_to_device(src: Path, dest: Path) -> None:
    """Copy without AppleDouble (._*) files, which the firmware would try to decode."""
    subprocess.run(["/bin/cp", "-X", str(src), str(dest)], check=True)
    subprocess.run(["dot_clean", "-m", str(dest.parent)], check=False)
    os.sync()
    if sha256(src) != sha256(dest):
        sys.exit(f"verify failed: {dest} does not match {src}")


# ---------------------------------------------------------------- commands

def cmd_status(args: argparse.Namespace) -> int:
    devs = usb_md3()
    mp = mount_point()
    out = {"usb": devs, "mount": str(mp) if mp else None}
    if mp:
        st = shutil.disk_usage(mp)
        out["free_bytes"], out["total_bytes"] = st.free, st.total
    if args.json:
        print(json.dumps(out, indent=2))
        return 0
    if not devs:
        print("MD3: not connected")
        return 1
    for d in devs:
        print(f"MD3: {d['mode']}  ({d['vid']:04x}:{d['pid']:04x} serial={d['serial']} at {d['location']})")
        if d["mode"] == "espressif-unknown":
            print("  unknown Espressif PID: possibly ROM download mode; try `esptool chip-id`")
    if mp:
        print(f"volume: {mp}  free {out['free_bytes'] / 1e6:.1f} MB of {out['total_bytes'] / 1e6:.1f} MB")
    return 0


def cmd_ls(args: argparse.Namespace) -> int:
    mp = require_mount()
    for folder in FOLDERS:
        d = mp / folder
        files = sorted(p for p in d.iterdir() if not p.name.startswith(".")) if d.exists() else []
        print(f"{folder}/")
        for p in files:
            s = gif_summary(p)
            meta = (f"{s['format']} {s['w']}x{s['h']} frames={s['frames']} hold={s['hold_last_ms']}ms"
                    if "error" not in s else s["error"])
            print(f"  {p.name:<24} {p.stat().st_size // 1024:6d} KB  {meta}")
    return 0


def cmd_backup(args: argparse.Namespace) -> int:
    mp = require_mount()
    dest = Path(args.dest) if args.dest else BACKUPS / datetime.now().strftime("%Y-%m-%d-%H%M%S")
    (dest / "files").mkdir(parents=True, exist_ok=True)
    subprocess.run(["/usr/bin/rsync", "-a", "--exclude", ".Spotlight-V100", "--exclude", ".fseventsd",
                    "--exclude", ".Trashes", "--exclude", "._*", f"{mp}/", str(dest / "files") + "/"],
                   check=True)
    lines = []
    for p in sorted((dest / "files").rglob("*")):
        if p.is_file():
            lines.append(f"{sha256(p)}  {p.relative_to(dest)}")
    (dest / "files.sha256").write_text("\n".join(lines) + "\n")
    print(f"backed up {len(lines)} files to {dest}")
    return 0


def cmd_convert(args: argparse.Namespace) -> int:
    out = convert(Path(args.src), Path(args.out), fit=args.fit, fps=args.fps,
                  hold_last_ms=args.hold, colors=args.colors)
    s = gif_summary(out)
    print(f"{out}  {out.stat().st_size // 1024} KB  frames={s['frames']}")
    if out.stat().st_size > SIZE_WARN:
        print("warning: over 2 MB; Fosi warns large GIFs may play poorly. Try --colors 64 or --fps 12.")
    return 0


def cmd_push(args: argparse.Namespace) -> int:
    mp = require_mount()
    if args.folder not in FOLDERS:
        sys.exit(f"folder must be one of {FOLDERS}")
    src = Path(args.src)
    name = args.name or (src.stem + ".gif")
    staged = PROJECT / "assets" / "staged" / name
    s = gif_summary(src)
    if (not args.raw) and (s.get("format") != "GIF" or (s.get("w"), s.get("h")) != (SIZE, SIZE)
                           or args.hold is not None):
        convert(src, staged, fit=args.fit, fps=args.fps, hold_last_ms=args.hold, colors=args.colors)
    else:
        staged.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, staged)
    dest = mp / args.folder / name
    if dest.exists() and not args.force:
        sys.exit(f"{dest} exists; pass --force to replace it")
    need = staged.stat().st_size - (dest.stat().st_size if dest.exists() else 0)
    if need > shutil.disk_usage(mp).free:
        sys.exit("not enough free space on the MD3")
    copy_to_device(staged, dest)
    print(f"pushed {name} -> {args.folder}/ ({staged.stat().st_size // 1024} KB, verified)")
    return 0


def cmd_rm(args: argparse.Namespace) -> int:
    mp = require_mount()
    target = mp / args.path
    if target.parent.name not in FOLDERS or not target.is_file():
        sys.exit(f"not a file inside one of {FOLDERS}: {args.path}")
    if not args.no_backup_check and not any(BACKUPS.glob(f"*/files/{args.path}")):
        sys.exit(f"{args.path} is not in any backup under {BACKUPS}; run `md3 backup` first "
                 f"or pass --no-backup-check")
    target.unlink()
    os.sync()
    print(f"removed {args.path}")
    return 0


def cmd_restore(args: argparse.Namespace) -> int:
    mp = require_mount()
    src = Path(args.backup) / "files"
    if not src.is_dir():
        sys.exit(f"no files/ directory in {args.backup}")
    for folder in FOLDERS:
        for p in sorted((src / folder).glob("*")):
            dest = mp / folder / p.name
            if not dest.exists() or sha256(dest) != sha256(p):
                copy_to_device(p, dest)
                print(f"restored {folder}/{p.name}")
        if args.exact:
            keep = {p.name for p in (src / folder).glob("*")}
            for extra in (mp / folder).glob("*"):
                if extra.name not in keep and not extra.name.startswith("."):
                    extra.unlink()
                    print(f"removed {folder}/{extra.name} (not in backup)")
    os.sync()
    return 0


def cmd_eject(args: argparse.Namespace) -> int:
    mp = require_mount()
    subprocess.run(["dot_clean", "-m", str(mp)], check=False)
    r = subprocess.run(["diskutil", "eject", str(mp)])
    if r.returncode == 0:
        print("ejected. On the MD3: Menu > USB Mode > Audio (DAC) Mode; replug to reload images.")
    return r.returncode


def main() -> int:
    ap = argparse.ArgumentParser(prog="md3", description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("status", help="which mode the MD3 is in, and the volume if mounted")
    p.add_argument("--json", action="store_true")
    p.set_defaults(fn=cmd_status)

    sub.add_parser("ls", help="list every folder with frame counts").set_defaults(fn=cmd_ls)

    p = sub.add_parser("backup", help="copy the whole volume to backup/<timestamp> with checksums")
    p.add_argument("dest", nargs="?")
    p.set_defaults(fn=cmd_backup)

    def media_opts(p: argparse.ArgumentParser) -> None:
        p.add_argument("--fit", choices=("cover", "contain"), default="cover")
        p.add_argument("--fps", type=float, default=None)
        p.add_argument("--hold", type=int, default=None, help="last-frame hold in ms (game result)")
        p.add_argument("--colors", type=int, default=128)

    p = sub.add_parser("convert", help="make a 240x240 MD3 GIF from any image or GIF")
    p.add_argument("src")
    p.add_argument("out")
    media_opts(p)
    p.set_defaults(fn=cmd_convert)

    p = sub.add_parser("push", help="convert if needed, copy to a folder, verify")
    p.add_argument("src")
    p.add_argument("--folder", default="Photo1")
    p.add_argument("--name")
    p.add_argument("--force", action="store_true")
    p.add_argument("--raw", action="store_true", help="copy as-is, no conversion")
    media_opts(p)
    p.set_defaults(fn=cmd_push)

    p = sub.add_parser("rm", help="remove Folder/file.gif (refuses unless it is backed up)")
    p.add_argument("path")
    p.add_argument("--no-backup-check", action="store_true")
    p.set_defaults(fn=cmd_rm)

    p = sub.add_parser("restore", help="put a backup's files back")
    p.add_argument("backup")
    p.add_argument("--exact", action="store_true", help="also delete files not in the backup")
    p.set_defaults(fn=cmd_restore)

    sub.add_parser("eject", help="clean AppleDouble files and eject").set_defaults(fn=cmd_eject)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
