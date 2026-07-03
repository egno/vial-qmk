# Aurora Corne OLED custom glyphs

Reference for `glcdfont.c` and `corne.c` modifier / icon layout.

## Font format

- **File:** `keyboards/splitkb/aurora/corne/glcdfont.c` (via `OLED_FONT_H` in `config.h`)
- **Glyph size:** 6×8 px (6 column bytes × 8 rows)
- **Array index:** line in `font[]` minus header lines — index `N` is array entry `N` (line `N + 10` in the `.c` file)
- **Char code:** custom glyphs use `0x80 + index` (index 128 → `0x80`)
- **Column byte:** MSB (bit 7) = top pixel, bit 0 = bottom pixel
- **Editor:** [Helix Font Editor](https://helixfonteditor.netlify.com/) or `tools/glcdfont_converter/`

## OLED layout (master half)

- Rotation: **270°** (`oled_init_kb` in `corne.c`)
- 128×32 panel → 5 columns × 16 text rows when rotated
- Mod status: two 2×2 icons side-by-side (GUI+Alt, then Ctrl+Shift), written as **top row first**, then bottom row (same order as Soundmonster Corne)

## Modifier icons (2×2 each)

Outline icons shown when mod **active**; blank when **inactive** (`corne.c`).

| Mod | Mac name | Top row (indices) | Bottom row (indices) | Char codes (top / bottom) |
|-----|----------|-------------------|----------------------|---------------------------|
| GUI | Command ⌘ | 133–134 | 165–166 | `0x85–0x86` / `0xA5–0xA6` |
| Alt | Option ⌥ | 135–136 | 167–168 | `0x87–0x88` / `0xA7–0xA8` |
| Ctrl | Control ⌃ | 137–138 | 169–170 | `0x89–0x8A` / `0xA9–0xAA` |
| Shift | Shift ⇧ | 139–140 | 171–172 | `0x8B–0x8C` / `0xAB–0xAC` |

Each icon is **12×16 px** (two 6×8 glyphs wide × two rows tall).

**Inactive:** `0x80` (index 128, all zeros) — used for both glyph cells and fillers.

**Unused:** indices 129–132 (`0x81–0x84`) — leftover slots before Command art; safe to blank or repurpose.

## Mouse icon (3×3)

Computer-mouse outline at indices **141–143**, **173–175**, **205–207** (`0x8D–0x8F`, `0xAD–0xAF`, `0xCD–0xCF`).

| Row | Indices | Char codes |
|-----|---------|------------|
| Top | 141–143 | `0x8D–0x8F` |
| Mid | 173–175 | `0xAD–0xAF` |
| Bot | 205–207 | `0xCD–0xCF` |

Total size **18×24 px**. Not wired in `corne.c` yet; to draw:

```c
static const char PROGMEM mouse_1[] = {0x8D, 0x8E, 0x8F, 0};
static const char PROGMEM mouse_2[] = {0xAD, 0xAE, 0xAF, 0};
static const char PROGMEM mouse_3[] = {0xCD, 0xCE, 0xCF, 0};
// oled_write_P(mouse_1, false); … newline … mouse_2 … mouse_3
```

**Orientation:** glyph art was **flipped vertically** (reverse bits in each column byte) for correct appearance on the rotated OLED. If it looks mirrored left/right, flip horizontally instead (reverse column byte order within each 6×8 glyph).

## Flipping glyphs

```python
FONTW = 6

def flip_h(glyph: bytes) -> bytes:
    """Mirror left ↔ right."""
    return bytes(glyph[FONTW - 1 - c] for c in range(FONTW))

def flip_v(glyph: bytes) -> bytes:
    """Mirror top ↔ bottom (MSB ↔ LSB in each column byte)."""
    out = bytearray()
    for b in glyph:
        nb = sum(((b >> i) & 1) << (7 - i) for i in range(8))
        out.append(nb)
    return bytes(out)
```

- **Horizontal flip:** swap which side of the icon the button divider / scroll tick appears on
- **Vertical flip:** swap dome vs tail within the 3×3 grid bands
- Swapping index **rows** (141↔205) is different from flipping inside a glyph

## Build

```bash
qmk compile -kb splitkb/aurora/corne/rev1 -km vial_full -e CONVERT_TO=liatris
```
