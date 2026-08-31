# wl-kbptr — Bisect Mode 5×5 Customization

> Handoff / working notes. Use this to resume work on the `bisect` mode of
> `wl-kbptr` with fresh context. Last state: a **5×5 (25-key) keyboard-accurate
> grid** is implemented, built, and installed locally.

## 1. What this project is

- **wl-kbptr** = "Wayland Keyboard Pointer": move/click the mouse with the keyboard.
- Repo (local): `~/Projects/wl-kbptr`
- Upstream: `https://github.com/moverest/wl-kbptr` (branch `main`, v0.4.1)
- Installed binary (the one actually launched by keybindings): `~/.local/bin/wl-kbptr`
- User config: `~/.config/wl-kbptr/config` (currently the stock default; `modes=tile,bisect`)
- Helper: `helpers/wl-kbptr-sway-active-win` → runs `wl-kbptr --restrict <active-window-rect>`

### Modes
`tile`, `bisect`, `split`, `floating`, `click`, chainable via `modes=...`.
Our work is entirely in **bisect**.

## 2. Current implemented state

Bisect now divides an area into a **5×5 grid** (25 selectable cells) using keys
laid out to mirror the physical keyboard. Thin areas still fall back to 2-way
splits (`DIVISION_HORIZONTAL` / `DIVISION_VERTICAL`), then `UNDIVIDABLE`.

### The 5×5 grid (screen cell = key)
Row-major: `idx = i + j*5` (i = column 0..4, j = row 0..4).

```
row0:  q  w  e  r  t      (top keyboard row, left half)   idx  0..4
row1:  y  u  i  o  p      (top keyboard row, right half)  idx  5..9
row2:  a  s  d  f  g      (home row, left half)           idx 10..14
row3:  h  j  k  l  ;      (home row, right half)          idx 15..19
row4:  z  x  c  v  b      (bottom row, left half)         idx 20..24
```

- **Contiguous**: every screen row is a contiguous run of 5 keyboard keys (no gaps).
- **Wrap**: each 10-key keyboard row is wrapped across two screen rows (left/right half).
- **Omitted**: the bottom-right keys `n m , . /` are intentionally left out.
- **Click keys** (not on the grid): `n` = left, `m` = right, `,` = middle.

## 3. How it actually works (the lever)

The on-screen key at cell `(i, j)` is simply `state->home_row[i + j*cols]`.
So **the entire grid layout is determined by the order of `key_codes` in
`load_home_row`** (`src/main.c`). Reordering that array reorders the grid — no
other layout logic needs to change.

- `state->home_row[0..24]` = the 25 selection keys, in the row-major order above.
- `state->home_row[25,26,27]` = the click keys `n, m, ,`.
- `find_str(home_row, ...)` resolves a pressed key to its index; `idx >= HOME_ROW_LEN` ⇒ click.

### Relevant files
| File | What to touch |
|------|---------------|
| `src/state.h` | `HOME_ROW_LEN` (25), `HOME_ROW_LEN_WITH_BTN` (28), `HOME_ROW_LEFT/RIGHT/MIDDLE_CLICK` (25/26/27), `BISECT_MAX_HISTORY` (16) |
| `src/main.c` | `load_home_row()` — the `key_codes[]` array (selection + click keys, in xkb keycodes) and the default `.home_row` literal in the `state` initializer |
| `src/mode_bisect.c` | `enum bisect_division`, `division_metrics[]` (`[DIVISION_25] = {5,5}`), `division_interfaces[]`, `determine_division()` (returns `DIVISION_25` for 2D areas), the forced-inner-label check (`division == DIVISION_25`) |
| `src/config.c` / `~/.config/wl-kbptr/config` | `home_row_keys` option overrides `load_home_row` if set (default empty ⇒ uses `key_codes`); bisect color/font settings |

### xkb keycodes used (for reference when editing `key_codes`)
Letters: q=0x18 w=0x19 e=0x1a r=0x1b t=0x1c y=0x1d u=0x1e i=0x1f o=0x20 p=0x21
a=0x26 s=0x27 d=0x28 f=0x29 g=0x2a h=0x2b j=0x2c k=0x2d l=0x2e ;=0x2f
z=0x34 x=0x35 c=0x36 v=0x37 b=0x38 n=0x39 m=0x3a ,=0x3b
(Note: xkb keycode = evdev + 8. The original code had `m` as `0x2f`, which is
actually `;` — we now use the correct `0x3a` for `m` and `0x2f` for `;`.)

## 4. Physical-logic design notes (why 5×5 / why wrap)

- "Physically logical" = screen position matches keyboard position on **both** axes.
- The keyboard letter block is **10 columns × 3 rows** (wide), but our grid is ~square. A square grid can't match both axes, so the least-bad choice is to **wrap each keyboard row across two screen rows** — this preserves the vertical axis (top row at top, home in middle, bottom at bottom) and the left→right order within each row. The only distortion: the right half of a keyboard row sits one screen-row lower.
- **Clean wrapping requires screen-columns to divide the keyboard's 10 columns.** 5 divides 10 (10/5 = 2 rows per keyboard row → boundaries align perfectly). **4 does NOT divide 10** (10/4 = 2.5) → row boundaries cut *mid-keyboard-row* → illogical. So **5×N is fundamentally more logical than 4×N**.
- Comparison we settled on:
  - **4×4 / 4×5** — illogical (4 doesn't divide 10; mid-row cuts).
  - **5×4 (20)** — *most* logically clean: full top + home rows, symmetric, no cuts. (Was tried, then reverted.)
  - **5×5 (25)** — same clean wrap + a bottom-left row; only blemish is `n m , . /` omitted. Chosen for the extra 5 options.
- Click keys: originally `g/h/b`, but `g,h,b` collide with the selection set in 5×5, so they moved to the free bottom-right keys `n / m / ,`.

## 5. Build / install / test

```bash
cd ~/Projects/wl-kbptr
meson compile -C build            # build/ already configured
rm -f ~/.local/bin/wl-kbptr && \
  cp build/wl-kbptr ~/.local/bin/wl-kbptr   # install (rm first; "text file busy" if a copy is running)
wl-kbptr --version                # sanity check
```

- **Test**: needs a running Wayland compositor (Sway/Hyprland). Launch via
  keybinding, or `wl-kbptr -o modes=tile,bisect`, or
  `wl-kbptr-sway-active-win` (active window). Cannot be tested headlessly here.
- **No compositor reload needed** — `wl-kbptr` is spawned fresh on each keypress.
  If an old instance is on screen, press `Escape` to exit it, then relaunch.
- **Build is without OpenCV.** Do not put `mode_floating.source=detect` in
  `~/.config/wl-kbptr/config`: the config then fails to parse (`Binary not
  build with OpenCV. 'detect' source not supported.`) and wl-kbptr exits
  before any `-o` override is applied, so keybindings launch nothing. See
  `SCROLL_MODE_NOTES.md` §5.
- If you get "text file busy" on `cp`, ensure no `wl-kbptr` process is running
  (`pgrep -fa wl-kbptr`) and retry.

## 6. Known limitations / dead code

- `DIVISION_8` and `DIVISION_4` are still defined in the enum / `division_metrics[]` /
  `division_interfaces[]` but are **never selected** (only `DIVISION_25` for 2D,
  plus the thin-area splits). `DIVIDE_8_RATIO` is now an unused `#define`.
  Upstream originally used 8-division (4×2) for very wide areas and 4×4 for squares.
- The grid size (5×5) is **hardcoded** as `DIVISION_25` — not configurable.
- If a user sets `home_row_keys=` in config, they must supply ≥28 keys (25 + 3)
  for 5×5, or indices will misalign.
- Local-only changes; **not committed** and **not pushed** upstream.

## 7. Possible next iterations

- **Make grid size configurable** (e.g. `mode_bisect.columns` / `rows`, or a
  `grid=5x5` setting) instead of the hardcoded `DIVISION_25`.
- **Adaptive division**: wide areas → 4×2 (square sub-cells), square-ish → 5×5,
  mirroring upstream's original intent but with the keyboard-accurate wrap.
- **Clean up dead code**: remove `DIVISION_8`/`DIVISION_4`/`DIVIDE_8_RATIO` or wire
  them back into an adaptive scheme.
- **Configurable click keys** (so users can pick their own without code edits).
- **Click-action mode** (upstream issue #88): a "choose action" step
  (left/right/middle/double/ask) after selecting — could integrate with bisect.
- **Upstream PR**: the keyboard-accurate wrap + relative-font-size for bisect
  (upstream issue #95) could be worth contributing back.
- **Multi-monitor**: verify pointer accuracy (upstream issue #56) if relevant.

## 8. Quick mental model for resuming

> To change which keys appear where on the bisect grid, edit the `key_codes[]`
> array order in `src/main.c:load_home_row`. To change grid *shape*, edit
> `division_metrics` / `determine_division` in `src/mode_bisect.c`. Then
> `meson compile -C build` and `cp build/wl-kbptr ~/.local/bin/`.
