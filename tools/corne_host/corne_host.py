#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Host side of the vial_full keymap's raw HID commands (Aurora Corne).

Protocol: keyboards/splitkb/aurora/corne/keymaps/vial_full/host_hid.h
Needs the hidapi bindings:  python3 -m pip install hidapi
"""
import argparse
import json
import re
import subprocess
import struct
import sys
import time
from pathlib import Path

# QMK raw HID interface.
USAGE_PAGE = 0xFF60
USAGE = 0x61
REPORT_SIZE = 32

# splitkb.com Aurora Corne rev1; refreshed from the keyboard's json when run from the repo.
VID = 0x8D1D
PID = 0x343A
KEYBOARD_DIR = Path(__file__).resolve().parents[2] / "keyboards/splitkb/aurora/corne"

CMD_VERSION = 0xF0
CMD_DRILLS = 0xF1
CMD_STATUS = 0xF2
CMD_MISFIRE = 0xF3
UNHANDLED = 0xFF

DRILLS_INFO, DRILLS_CLEAR, DRILLS_ADD, DRILLS_MODE = range(4)
MISFIRE_GET, MISFIRE_RESET = range(2)
DRILL_ERRORS = {1: "host set is full", 2: "bad length", 3: "untypeable character", 4: "duplicate"}

STATUS_ROWS = 3
STATUS_COLS = 5
DEFAULT_TTL = 90


def usb_ids():
    vid, pid = VID, PID
    try:
        usb = {}
        for name in ("info.json", "rev1/keyboard.json"):
            usb.update(json.loads((KEYBOARD_DIR / name).read_text()).get("usb", {}))
        vid, pid = int(usb.get("vid", hex(vid)), 16), int(usb.get("pid", hex(pid)), 16)
    except (OSError, ValueError):
        pass  # copied out of the repo: use the built-in ids
    return vid, pid


class Keyboard:
    def __init__(self):
        try:
            import hid
        except ImportError:
            sys.exit("Python hidapi bindings not found. Install them with: python3 -m pip install hidapi")
        vid, pid = usb_ids()
        paths = [d["path"] for d in hid.enumerate(vid, pid) if d.get("usage_page") == USAGE_PAGE and d.get("usage") == USAGE]
        if not paths:
            sys.exit(f"No raw HID interface found for {vid:04X}:{pid:04X} (is the left half plugged in?)")
        if hasattr(hid, "device"):  # "hidapi" package
            self.dev = hid.device()
            self.dev.open_path(paths[0])
            self._read = lambda: self.dev.read(REPORT_SIZE, 1000)
        else:  # "hid" package
            self.dev = hid.Device(path=paths[0])
            self._read = lambda: self.dev.read(REPORT_SIZE, timeout=1000)

    def request(self, *payload):
        data = bytes(payload)
        if len(data) > REPORT_SIZE:
            raise ValueError("request does not fit one report")
        data = data.ljust(REPORT_SIZE, b"\0")
        self.dev.write(b"\0" + data)  # leading report id
        reply = bytes(self._read() or b"")
        if len(reply) < REPORT_SIZE:
            sys.exit("No reply from the keyboard.")
        if reply[0] == UNHANDLED:
            sys.exit(f"The firmware does not know command 0x{data[0]:02X}/{data[1]} (old build?).")
        if reply[0] != data[0]:
            sys.exit(f"Unexpected reply: {reply.hex()}")
        return reply

    # -- commands ----------------------------------------------------------

    def version(self, field):
        return self.request(CMD_VERSION, field)[2:].split(b"\0")[0].decode("ascii", "replace")

    def drills(self, sub, *args):
        r = self.request(CMD_DRILLS, sub, *args)
        return {"status": r[2], "capacity": r[3], "count": r[4], "max_len": r[5], "only": bool(r[6]), "bad_pos": r[7], "bad_char": r[8]}

    def status(self, rows, invert=0, ttl=DEFAULT_TTL):
        text = "".join(row[:STATUS_COLS].ljust(STATUS_COLS) for row in rows[:STATUS_ROWS])
        text = text.ljust(STATUS_ROWS * STATUS_COLS).encode("ascii", "replace")
        self.request(CMD_STATUS, ttl, invert, *text)


    def misfire(self, sub, index=0):
        r = self.request(CMD_MISFIRE, sub, index)
        keycode, held, tapped, holds, taps = struct.unpack_from("<HHHII", r, 7)
        return {"ok": r[2] == 0, "count": r[3], "row": r[5], "col": r[6], "keycode": keycode,
                "held": held, "tapped": tapped, "holds": holds, "taps": taps}


def describe(info):
    mode = "host items only" if info["only"] else "mixed with built-in items"
    return f"{info['count']}/{info['capacity']} host items loaded, max {info['max_len']} chars each, {mode}"


def cmd_version(kb, args):
    for field, name in enumerate(("version", "built", "commit")):
        print(f"{name:8}{kb.version(field)}")


MOD_NAMES = ("Ctrl", "Shift", "Alt", "GUI")


def mod_tap_label(keycode):
    """Name a QMK mod-tap keycode (0x2000-0x3FFF: 5 mod bits, then the tap keycode)."""
    if not 0x2000 <= keycode <= 0x3FFF:
        return f"0x{keycode:04X}"
    tap, mods = keycode & 0xFF, (keycode >> 8) & 0x1F
    if 0x04 <= tap <= 0x1D:
        key = chr(ord("A") + tap - 0x04)
    else:
        key = {0x2B: "Tab", 0x2C: "Space", 0x28: "Enter", 0x29: "Esc", 0x2A: "Bksp"}.get(tap, f"0x{tap:02X}")
    side = "R" if mods & 0x10 else "L"
    held = "+".join(side + name for bit, name in enumerate(MOD_NAMES) if mods & (1 << bit))
    return f"{key} ({held})"


def rate(misfires, total):
    return f"{100 * misfires / total:5.1f}%" if total else "     -"


def cmd_misfire(kb, args):
    if args.reset:
        kb.misfire(MISFIRE_RESET)
        print("Misfire counters reset.")
        return
    count = kb.misfire(MISFIRE_GET)["count"]
    print(f"{'key':14}{'pos':6}{'holds':>8}{'bad':>6}{'rate':>8}{'taps':>9}{'bad':>6}{'rate':>8}")
    for index in range(count):
        e = kb.misfire(MISFIRE_GET, index)
        if not e["ok"]:
            break
        print(f"{mod_tap_label(e['keycode']):14}{str(e['row']) + ',' + str(e['col']):6}"
              f"{e['holds']:8}{e['held']:6}{rate(e['held'], e['holds']):>8}"
              f"{e['taps']:9}{e['tapped']:6}{rate(e['tapped'], e['taps']):>8}")
    print("bad = followed by Backspace; tapped counts include ordinary typos, so compare keys with each other.")


def cmd_drills_info(kb, args):
    print(describe(kb.drills(DRILLS_INFO)))


def cmd_drills_clear(kb, args):
    print(describe(kb.drills(DRILLS_CLEAR)))


def cmd_drills_mode(kb, args):
    print(describe(kb.drills(DRILLS_MODE, args.mode == "host")))


def cmd_drills_push(kb, args):
    items = [line.rstrip() for line in Path(args.file).read_text().splitlines()]
    items = [item for item in items if item]
    info = kb.drills(DRILLS_INFO if args.append else DRILLS_CLEAR)
    accepted = 0
    for item in items:
        try:
            raw = item.encode("ascii")
        except UnicodeEncodeError:
            print(f"rejected  {item!r}: not ASCII")
            continue
        if len(raw) > info["max_len"]:
            print(f"rejected  {item!r}: longer than {info['max_len']} chars")
            continue
        r = kb.drills(DRILLS_ADD, len(raw), *raw)
        if r["status"] == 0:
            accepted += 1
            print(f"accepted  {item!r}")
            continue
        reason = DRILL_ERRORS.get(r["status"], f"error {r['status']}")
        if r["status"] == 3:
            reason = f"{chr(r['bad_char'])!r} at position {r['bad_pos'] + 1} cannot be typed on the current keymap"
        print(f"rejected  {item!r}: {reason}")
    if args.only is not None:
        kb.drills(DRILLS_MODE, args.only)
    print(f"{accepted} accepted, {len(items) - accepted} rejected; {describe(kb.drills(DRILLS_INFO))}")


def invert_mask(rows):
    return sum(1 << (r - 1) for r in set(rows or []))


def cmd_status_set(kb, args):
    kb.status(args.text, invert_mask(args.invert), args.ttl)


def cmd_status_clear(kb, args):
    kb.status([], ttl=0)


def macos_input_source():
    """Short tag for the current macOS input source ("EN", "RU", ...), "" if unknown."""
    try:
        out = subprocess.run(["defaults", "read", "com.apple.HIToolbox", "AppleSelectedInputSources"], capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return ""
    m = re.search(r'"KeyboardLayout Name" = "?([^";]+)"?;', out) or re.search(r'"Input Mode" = "?(?:[\w.]*\.)?([^";.]+)"?;', out)
    if not m:
        return ""
    name = m.group(1).strip()
    if name in ("ABC", "U.S.", "US", "British", "Australian", "Canadian", "USInternational-PC", "Colemak", "Dvorak"):
        return "EN"
    return re.sub(r"[^A-Za-z]", "", name)[:2].upper()


def macos_mic_muted():
    try:
        out = subprocess.run(["osascript", "-e", "input volume of (get volume settings)"], capture_output=True, text=True, timeout=5).stdout
        return int(out.strip()) == 0
    except (OSError, ValueError, subprocess.SubprocessError):
        return False


def cmd_status_watch(kb, args):
    """Clock on row 1, input source on row 2 (--layout), MIC on row 3, inverted while muted (--mic)."""
    last, last_push = None, 0.0
    try:
        while True:
            rows = [time.strftime("%H:%M"), macos_input_source() if args.layout else "", "MIC" if args.mic else ""]
            invert = 0b100 if args.mic and macos_mic_muted() else 0
            now = time.monotonic()
            # Push on change, and well inside the TTL so the text never lapses while we run.
            if (rows, invert) != last or now - last_push >= args.ttl / 3:
                kb.status(rows, invert, args.ttl)
                last, last_push = (rows, invert), now
            time.sleep(args.interval)
    except KeyboardInterrupt:
        kb.status([], ttl=0)
        print()


def ttl_arg(value):
    ttl = int(value)
    if not 1 <= ttl <= 255:
        raise argparse.ArgumentTypeError("ttl must be 1..255 seconds")
    return ttl


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("version", help="firmware version, build date and commit").set_defaults(fn=cmd_version)

    drills = sub.add_parser("drills", help="host-fed trainer drills (RAM only)").add_subparsers(dest="sub", required=True)
    push = drills.add_parser("push", help="load drill items from a file, one per line")
    push.add_argument("file")
    push.add_argument("--append", action="store_true", help="keep the items already loaded (default: replace them)")
    only = push.add_mutually_exclusive_group()
    only.add_argument("--only", dest="only", action="store_const", const=1, help="then drill host items only")
    only.add_argument("--mixed", dest="only", action="store_const", const=0, help="then mix with the built-in items")
    push.set_defaults(fn=cmd_drills_push, only=None)
    drills.add_parser("clear", help="drop all host items").set_defaults(fn=cmd_drills_clear)
    drills.add_parser("info", help="capacity, count and mode").set_defaults(fn=cmd_drills_info)
    mode = drills.add_parser("mode", help="drill host items only, or mix them with the built-in ones")
    mode.add_argument("mode", choices=("host", "mixed"))
    mode.set_defaults(fn=cmd_drills_mode)

    status = sub.add_parser("status", help="text on the right-half OLED").add_subparsers(dest="sub", required=True)
    st_set = status.add_parser("set", help=f"show up to {STATUS_ROWS} rows of {STATUS_COLS} chars, one argument per row")
    st_set.add_argument("text", nargs="+")
    st_set.add_argument("--invert", type=int, action="append", choices=range(1, STATUS_ROWS + 1), metavar="ROW", help="draw this row inverted (repeatable)")
    st_set.add_argument("--ttl", type=ttl_arg, default=DEFAULT_TTL, help="seconds until the keyboard drops the text (default %(default)s)")
    st_set.set_defaults(fn=cmd_status_set)
    status.add_parser("clear", help="remove the text now").set_defaults(fn=cmd_status_clear)
    watch = status.add_parser("watch", help="keep a clock on the OLED until interrupted")
    watch.add_argument("--layout", action="store_true", help="macOS: also show the current input source (EN/RU/...)")
    watch.add_argument("--mic", action="store_true", help="macOS: also show MIC, inverted while the input is muted")
    watch.add_argument("--interval", type=float, default=5, help="seconds between checks (default %(default)s)")
    watch.add_argument("--ttl", type=ttl_arg, default=DEFAULT_TTL, help="seconds the text survives without this script (default %(default)s)")
    watch.set_defaults(fn=cmd_status_watch)

    misfire = sub.add_parser("misfire", help="mod-tap misfire counters gathered during normal typing")
    misfire.add_argument("--reset", action="store_true", help="zero the counters instead of printing them")
    misfire.set_defaults(fn=cmd_misfire)

    args = p.parse_args()
    if args.cmd == "status" and args.sub == "set" and len(args.text) > STATUS_ROWS:
        p.error(f"at most {STATUS_ROWS} rows")
    args.fn(Keyboard(), args)


if __name__ == "__main__":
    main()
