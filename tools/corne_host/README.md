# corne_host

Host-side tool for the raw HID commands of the Aurora Corne `vial_full` keymap
(`keyboards/splitkb/aurora/corne/keymaps/vial_full`). The protocol is documented in
`host_hid.h` in that directory.

## Install

The only dependency is the Python hidapi binding:

    python3 -m pip install hidapi

(Use a virtualenv if you do not want it in your user site: `python3 -m venv .venv && .venv/bin/pip install hidapi`.)

The keyboard is found by VID/PID (read from the keyboard's `info.json` / `rev1/keyboard.json`, with
`8D1D:343A` built in as a fallback) and the QMK raw HID usage page `0xFF60`, usage `0x61`. Plug in
the left half: raw HID ends on the USB (master) side. Vial can stay open at the same time.

## Commands

    corne_host.py version                     firmware version, build date, commit

    corne_host.py drills push FILE            replace the host drill set with the lines of FILE
    corne_host.py drills push FILE --append   add to the loaded set instead
    corne_host.py drills push FILE --only     ...and drill host items only (--mixed switches back)
    corne_host.py drills mode host|mixed      switch the mode without pushing
    corne_host.py drills info                 capacity / count / mode
    corne_host.py drills clear                drop all host items

    corne_host.py status set 14:05 EN MIC --invert 3
    corne_host.py status clear
    corne_host.py status watch [--layout] [--mic]

    corne_host.py misfire                     per-key mod-tap misfire counters (RAM only)
    corne_host.py misfire --reset             zero them

    corne_host.py state [--watch]             layers, layer lock, mods, one-shots, trainer state
    corne_host.py trainer-stats [--items] [--all]
                                              last trainer session, per-character (and per-item) stats
    corne_host.py taphold [LAYER]             keys exempt from Chordal Hold / left alone by Flow Tap

    corne_host.py events watch [--ttl N] [--no-keys]
                                              pushed state and pressed-key events until Ctrl-C
    corne_host.py events get                  the same two reports once, without subscribing
    corne_host.py events off                  end a subscription now

### Drills

One item per line, printable ASCII, at most 24 characters; blank lines are skipped and trailing
whitespace is dropped. The keyboard holds up to 64 host items in RAM; they are gone after a power
cycle. Each line is reported as accepted or rejected. The firmware rejects an item when it is too
long, a duplicate, the set is full, or it contains a character no key on the current keymap can
type (the message names the character).

Host items form their own group next to the built-in ones and are picked by the same weighting
(slow / error-prone characters and never-typed items come up more often). In `host` mode only host
items are picked, as long as at least one of them is typeable; otherwise the trainer falls back to
the full set.

### Status text

Three rows of five characters on the right-half OLED, below the WPM graph. The keyboard drops the
text `--ttl` seconds (default 90, max 255) after the last update, so it does not go stale when the
script stops. `status watch` shows the clock on row 1 and refreshes it well inside the TTL; on macOS
`--layout` adds the input source on row 2 (via `defaults read com.apple.HIToolbox`) and `--mic` adds
`MIC` on row 3, inverted while the input volume is 0 (via `osascript`). Ctrl-C clears the text.

### Live state, trainer statistics, tap-hold info

These three only read (`0xF4`-`0xF6` in `host_hid.h`); answering them the keyboard never sends
anything unasked, so polling them next to an open Vial is safe.

`state` prints one line: highest layer, active / default / locked layers, held and one-shot mods,
Caps Word / Caps Lock, the trainer state and how many trainer sessions were finished since power-up.
`--watch` polls every `--interval` seconds (default 0.1) and prints the line whenever it changes.

`trainer-stats` prints the summary of the last finished session (WPM, accuracy, misses by kind), the
all-time best from EEPROM, and the per-character table: attempts, misses and mean latency, worst miss
rate first (`--all`: ASCII order). `--items` adds the per-item table. Everything but the all-time
record lives in RAM and is cumulative since power-up; the session counter shown by `state` tells a
polling host when a new summary is worth fetching.

`taphold` draws the matrix in physical order three times: keys exempt from the Chordal Hold same-hand
rule, tap-hold keys (`x` = Flow Tap never forces this one to tap), and keys that take part in Flow
Tap. Without `LAYER` it describes the keys as they resolve with the layers active right now. The
masks come from the table and function the firmware itself decides with (`hrm.c`).

### Events

`0xF7` in `host_hid.h`: instead of being polled the keyboard sends a state event (the payload of
`state`) whenever that state changes, and a matrix event (which switches are physically down, as
`row,col` of the 8x6 matrix: rows 0-3 left half, 4-7 right half) whenever a key goes down or up.
`events watch` subscribes, renews the subscription every `--ttl`/3 seconds (default ttl 5, max 255)
and prints both; each renewal makes the keyboard repeat the current state and keys, so identical lines
show up at that rate. Ctrl-C unsubscribes. If the script is killed the keyboard stops sending `--ttl`
seconds later at most; it also stops on USB suspend or re-plug.

The matrix event is a privacy matter: while a subscription runs, every program that has the raw HID
interface open can see which keys are pressed. Hence it is opt-in and expires; `--no-keys` only hides
the lines here, the keyboard still sends them. There is one subscription for all programs, and Vial
can mistake an event for the reply to one of its own requests, so do not leave `events watch` running
while you edit the keymap in Vial.
