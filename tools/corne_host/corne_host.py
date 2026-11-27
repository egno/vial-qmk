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
CMD_STATE = 0xF4
CMD_TRAINER = 0xF5
CMD_TAPHOLD = 0xF6
UNHANDLED = 0xFF

DRILLS_INFO, DRILLS_CLEAR, DRILLS_ADD, DRILLS_MODE = range(4)
MISFIRE_GET, MISFIRE_RESET = range(2)
TRAINER_SUMMARY, TRAINER_CHARS, TRAINER_ITEM, TRAINER_TEXT = range(4)
TRAINER_STATES = ("off", "session running", "results screen", "on, nothing typeable")
ITEM_GROUPS = ("pairs", "snippets", "numbers", "host")
MISS_KINDS = ("wrong key", "held", "tapped", "wrong layer")
TAPHOLD_CURRENT = 0xFF
MATRIX_ROWS, MATRIX_COLS = 8, 6
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

    def state(self):
        r = self.request(CMD_STATE)
        highest, layers, default, locked, flags, trainer, sessions, osm, osm_locked, osl, mods, weak, count = struct.unpack_from("<BIIIBBHBBBBBB", r, 3)
        return {"highest": highest, "layers": layers, "default": default, "locked": locked,
                "trainer_active": bool(flags & 1), "caps_word": bool(flags & 2), "caps_lock": bool(flags & 4),
                "trainer_state": trainer, "sessions": sessions, "oneshot_mods": osm, "oneshot_locked_mods": osm_locked,
                "oneshot_layer": None if osl == 0xFF else osl, "mods": mods, "weak_mods": weak, "layer_count": count}

    def trainer_summary(self):
        r = self.request(CMD_TRAINER, TRAINER_SUMMARY)
        f = struct.unpack_from("<BHBBBHH4HIBBHBB", r, 3)
        return {"have_session": r[2] == 0, "trainer_state": f[0], "sessions": f[1], "wpm": f[2], "accuracy": f[3],
                "items": f[4], "correct": f[5], "misses": f[6], "miss_kinds": f[7:11], "active_ms": f[11],
                "best_wpm": f[12], "best_accuracy": f[13], "total_sessions": f[14], "chars": f[15], "item_count": f[16]}

    def trainer_chars(self):
        """{char: (correct, misses, latency_ms)} for every printable ASCII character."""
        stats, index, total = {}, 0, 1
        while index < total:
            r = self.request(CMD_TRAINER, TRAINER_CHARS, index)
            total, n = r[3], r[5]
            if r[2] != 0 or n == 0:
                break
            for k in range(n):
                stats[chr(0x20 + index + k)] = struct.unpack_from("<HHI", r, 6 + 8 * k)
            index += n
        return stats

    def trainer_item(self, index):
        r = self.request(CMD_TRAINER, TRAINER_ITEM, index)
        if r[2] != 0:
            return None
        group, _, completions, misses, time_ms = struct.unpack_from("<BBHHI", r, 5)
        t = self.request(CMD_TRAINER, TRAINER_TEXT, index)
        text = t[6:6 + t[5]].decode("ascii", "replace") if t[2] == 0 else ""
        return {"group": group, "text": text, "completions": completions, "misses": misses, "time_ms": time_ms}

    def taphold(self, layer=TAPHOLD_CURRENT):
        r = self.request(CMD_TAPHOLD, layer)
        mask = lambda at: int.from_bytes(r[at:at + 6], "little")
        return {"ok": r[2] == 0, "chordal": bool(r[3] & 1), "flow": bool(r[3] & 2), "flow_term": struct.unpack_from("<H", r, 4)[0],
                "rows": r[6], "cols": r[7], "exempt": mask(8), "right": mask(14), "tap_hold": mask(20), "flow_keys": mask(26)}


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


def bits(mask):
    return [n for n in range(32) if mask & (1 << n)]


def mods_label(mask):
    names = [side + name for side, shift in (("L", 0), ("R", 4)) for bit, name in enumerate(MOD_NAMES) if mask & (1 << (bit + shift))]
    return "+".join(names) or "-"


def print_state(s):
    osl = "-" if s["oneshot_layer"] is None else s["oneshot_layer"]
    flags = [name for name, on in (("caps word", s["caps_word"]), ("caps lock", s["caps_lock"])) if on]
    print(f"layer {s['highest']}  on {bits(s['layers'])}  default {bits(s['default'])}  locked {bits(s['locked'])}  "
          f"mods {mods_label(s['mods'])}  weak {mods_label(s['weak_mods'])}  "
          f"one-shot {mods_label(s['oneshot_mods'])} locked {mods_label(s['oneshot_locked_mods'])} layer {osl}  "
          f"trainer {TRAINER_STATES[s['trainer_state']] if s['trainer_state'] < len(TRAINER_STATES) else s['trainer_state']} "
          f"({s['sessions']} sessions since power-up)  {' '.join(flags)}".rstrip())


def cmd_state(kb, args):
    if not args.watch:
        print_state(kb.state())
        return
    last = None
    try:
        while True:
            s = kb.state()
            if s != last:
                print_state(s)
                last = s
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print()


def cmd_trainer_stats(kb, args):
    s = kb.trainer_summary()
    print(f"trainer: {TRAINER_STATES[s['trainer_state']] if s['trainer_state'] < len(TRAINER_STATES) else s['trainer_state']}; "
          f"{s['sessions']} sessions since power-up, {s['total_sessions']} ever; best {s['best_wpm']} wpm, {s['best_accuracy']}%")
    if s["have_session"]:
        kinds = ", ".join(f"{name} {n}" for name, n in zip(MISS_KINDS, s["miss_kinds"]))
        print(f"last session: {s['wpm']} wpm, {s['accuracy']}%, {s['items']} items, {s['correct']} correct, "
              f"{s['misses']} misses ({kinds}), {s['active_ms'] / 1000:.1f} s")
    else:
        print("last session: none finished since power-up")
    chars = {ch: st for ch, st in kb.trainer_chars().items() if st[0] or st[1]}
    if chars:
        print(f"\n{'char':6}{'attempts':>9}{'misses':>8}{'miss %':>8}{'avg ms':>8}")
        order = sorted(chars) if args.all else sorted(chars, key=lambda ch: (-chars[ch][1] / (chars[ch][0] + chars[ch][1]), ch))
        for ch in order:
            correct, misses, latency = chars[ch]
            avg = f"{latency / correct:8.0f}" if correct else "       -"
            print(f"{ch!r:6}{correct + misses:9}{misses:8}{100 * misses / (correct + misses):8.1f}{avg}")
    if args.items:
        print(f"\n{'item':26}{'group':10}{'done':>6}{'misses':>8}{'avg ms':>8}")
        for index in range(s["item_count"]):
            item = kb.trainer_item(index)
            if item is None:
                break
            if not item["completions"] and not args.all:
                continue
            group = ITEM_GROUPS[item["group"]] if item["group"] < len(ITEM_GROUPS) else str(item["group"])
            avg = f"{item['time_ms'] / item['completions']:8.0f}" if item["completions"] else "       -"
            print(f"{item['text']:26}{group:10}{item['completions']:6}{item['misses']:8}{avg}")


def cmd_taphold(kb, args):
    t = kb.taphold(TAPHOLD_CURRENT if args.layer is None else args.layer)
    if not t["ok"]:
        sys.exit(f"The keyboard has no layer {args.layer}.")
    print(f"Chordal Hold {'on' if t['chordal'] else 'off'}, Flow Tap {str(t['flow_term']) + ' ms' if t['flow'] else 'off'}; "
          f"{'layers active now' if args.layer is None else 'layer ' + str(args.layer)}")
    half = t["rows"] // 2
    for name, mask, mark in (("exempt from the Chordal Hold same-hand rule", t["exempt"], "*"),
                             ("tap-hold keys (T) and of those the ones Flow Tap never forces to tap (x)", t["tap_hold"], "T"),
                             ("take part in Flow Tap", t["flow_keys"], "f")):
        print(f"\n{name}:")
        for row in range(half):
            line = ""
            # Physical order: the right half's columns run from the outside in.
            for r, cols in ((row, range(t["cols"])), (row + half, reversed(range(t["cols"])))):
                for c in cols:
                    pos = r * t["cols"] + c
                    cell = mark if mask & (1 << pos) else "."
                    if mark == "T" and cell == "T" and not t["flow_keys"] & (1 << pos):
                        cell = "x"
                    line += " " + cell
                line += "   "
            print(" " + line.rstrip())


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

    state = sub.add_parser("state", help="live state: layers, layer lock, mods, one-shots, trainer")
    state.add_argument("--watch", action="store_true", help="keep polling and print every change until interrupted")
    state.add_argument("--interval", type=float, default=0.1, help="seconds between polls with --watch (default %(default)s)")
    state.set_defaults(fn=cmd_state)

    stats = sub.add_parser("trainer-stats", help="last trainer session and per-character stats (RAM only)")
    stats.add_argument("--items", action="store_true", help="also list the per-item stats")
    stats.add_argument("--all", action="store_true", help="characters in ASCII order instead of worst first; with --items also the never-completed ones")
    stats.set_defaults(fn=cmd_trainer_stats)

    taphold = sub.add_parser("taphold", help="which keys Chordal Hold and Flow Tap leave alone")
    taphold.add_argument("layer", nargs="?", type=int, help="keymap layer (default: the layers active right now)")
    taphold.set_defaults(fn=cmd_taphold)

    args = p.parse_args()
    if args.cmd == "status" and args.sub == "set" and len(args.text) > STATUS_ROWS:
        p.error(f"at most {STATUS_ROWS} rows")
    args.fn(Keyboard(), args)


if __name__ == "__main__":
    main()
