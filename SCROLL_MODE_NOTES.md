# wl-kbptr — Scroll Mode + Multi-Instance Fix: Devlog

> Handoff / working notes for the `scroll` mode feature and the multi-instance
> bug. Use this to resume with fresh context. Last state (2026-08-30):
> scroll mode, the 5x5 bisect grid and the single-instance guard all build,
> are installed, and are verified working on the live box. The
> "invisible / undismissable overlay" regression is **resolved** — see §4 for
> the root cause, which is worth reading before touching `struct state`.

## 0. Quick orientation

- **Repo**: `~/Projects/wl-kbptr` (local copy of `moverest/wl-kbptr` v0.4.1).
- **Installed binary**: `~/.local/bin/wl-kbptr` (this is what keybindings launch; verify with `which -a wl-kbptr`).
- **Compositor**: Hyprland **0.56.2** (NOT Sway). `swaymsg` does **not** exist; use `hyprctl`.
- **User config**: `~/.config/wl-kbptr/config` (`modes=tile,bisect` originally; scroll added).
- **Hypr config**: `~/.config/hypr/` (Lua files: `bindings.lua`, `input.lua`, ...).

### Build / install / test
```bash
cd ~/Projects/wl-kbptr
meson compile -C build
pkill -x wl-kbptr; sleep 0.3
rm -f ~/.local/bin/wl-kbptr && cp build/wl-kbptr ~/.local/bin/wl-kbptr
wl-kbptr --version
```
- Needs a running Wayland compositor. Launched via keybinding or
  `wl-kbptr -o modes=...`. Cannot be tested headlessly.
- No compositor reload needed for the binary (spawned fresh each keypress);
  `hyprctl reload` only needed for config/binding changes.

---

## 1. Scroll mode — what was built

A new `scroll` mode that scrolls the surface under the pointer with `J`/`K`
(vertical) and `H`/`L` (horizontal). It is registered in the mode list and
configurable via `mode_scroll.step`.

### Files touched
| File | Change |
|------|--------|
| `src/mode_scroll.c | **new** — the scroll mode (enter/key/render/free). |
| `src/utils_wayland.h` | `scroll_pointer()` declaration. |
| `src/utils_wayland.c` | `scroll_pointer()` impl + `now_ms()` helper. |
| `src/config.h` | `struct mode_scroll_config { double step; }` + field in `config`. |
| `src/config.c` | `MSCR_FIELD` macro + `SECTION(mode_scroll, step "1")`. |
| `src/state.h` | `state.scroll` slot (pending/axis/value/x/y) for the queued scroll. |
| `src/main.c` | queue processed post-overlay; `ensure_single_instance()`; includes. |
| `meson.build` | added `src/mode_scroll.c`. |
| `helpers/wl-kbptr-hypr-active-win` | **new** — Hyprland equivalent of the Sway helper (uses `hyprctl`). |
| `~/.config/hypr/bindings.lua` | `SUPER+NTILDE` → hypr helper `modes=scroll,click`. |
| `~/.config/hypr/input.lua` | `emulate_discrete_scroll = 2`. |

### How it works NOW (a live mode — press repeatedly, `Esc` to leave)
Each `J`/`K`/`H`/`L` press calls `scroll_pointer()` immediately and the mode
**stays up**, HUD and all. There is no queue: the old `state.scroll` slot and
the post-teardown flush in `main.c` are gone.

Making this work needed two things, both inside `scroll_pointer()`
(`utils_wayland.c`) — see §2 Bug B for why:
1. **Drop the exclusive keyboard grab for the duration of the send.** While a
   layer surface holds `KEYBOARD_INTERACTIVITY_EXCLUSIVE`, Hyprland routes all
   pointer input to it and **ignores the empty input region**, so the axis
   lands on our own overlay.
2. **Nudge the cursor by one pixel before moving it to the target.** A
   `motion_absolute` to the position the cursor already occupies is a no-op,
   so pointer focus is never recomputed and the axis keeps going wherever it
   went last. This is what made scroll #2 onwards silently do nothing.

**Do not add roundtrips to that sequence.** Ungrab, nudge, move, axis and
re-grab are issued as one ordered, unflushed request batch with a *single*
`wl_display_roundtrip()` at the end, so the compositor applies them back to
back and never sits ungrabbed waiting on input. With 6 roundtrips instead of 1
the hit rate was 4/6 and keypresses leaked to the window underneath; with 1 it
is 8/8.

Keybindings:
- `CTRL + NTILDE` → `wl-kbptr -o modes=bisect,click` (unchanged, worked before).
- `SUPER + NTILDE` → `~/Projects/wl-kbptr/helpers/wl-kbptr-hypr-active-win -o modes=scroll,click` (scrolls the active window's center).

Scroll keys: `J`/`K` = down/up, `H`/`L` = left/right. `Esc` quit,
`BackSpace` back, `Enter`/`Space` next mode.

### `mode_scroll.step` — scroll magnitude
`step` is the number of **mouse-wheel notches** sent per keypress (default 1,
currently `3` in `~/.config/wl-kbptr/config`). Config-only: no rebuild needed,
and `-o mode_scroll.step=N` overrides it for one run.

`scroll_pointer()` emits `step` *separate* notch frames rather than one event
with `discrete = step` — some clients count the latter as a single click. The
continuous `axis` value is `15.0` per notch (`WHEEL_NOTCH`), the conventional
delta a real wheel reports; it used to send `step` (i.e. `1.0` for one notch),
which only went unnoticed because `emulate_discrete_scroll = 2` makes Hyprland
take the discrete path and ignore it.

Measured on this box (foot terminal, one `k` press). Scaling is **linear**:

| step | lines scrolled |
|-----:|---------------:|
|    1 |             11 |
|    3 |             33 |
|    5 |             56 |
|   10 |            112 |
|   15 |            168 |
|   20 |            224 |
|   30 |            336 |

~11 lines per notch is `foot`'s own scroll multiplier stacked on
`emulate_discrete_scroll = 2`; a browser moves a different absolute distance
per notch, but the linear scaling holds, so `step` is a predictable multiplier
everywhere. Judge the feel in the target app, not from these line counts.

---

## 2. Hyprland virtual-pointer scroll — investigation (root causes found in source)

Reference: read `Hyprland` src at tag `v0.56.2`:
- `src/protocols/VirtualPointer.cpp`
- `src/managers/input/InputManager.cpp`

### Bug A — `axis_discrete` value is multiplied by 120
`VirtualPointer.cpp` `setAxisDiscrete`:
```c
m_axisEvents[m_axis].deltaDiscrete = discrete * 120;
```
So sending `discrete=120` becomes `deltaDiscrete = 14400` (a giant scroll).
**Fix**: send `axis_discrete` with `discrete = 1` (step), which Hyprland turns
into `120` = exactly one logical notch. (`scroll_pointer` computes
`discrete = lround(wl_fixed_to_double(value))`, i.e. the step magnitude.)

### Bug B — axis delivered to wl-kbptr's own overlay surface
The cause is the **exclusive keyboard grab**, not the layer or the stacking
order: an exclusive-keyboard layer surface takes pointer focus too, and the
empty input region does not exempt it.

**Original workaround** (superseded): send the axis after the overlay is
destroyed, making scroll one-shot.

**Current fix**: briefly drop the grab around the send, and nudge the cursor so
pointer focus is recomputed (§1). Measured 8/8, then 5/5 end to end.

Two alternatives were tried on the live box and **rejected**:
- `KEYBOARD_INTERACTIVITY_ON_DEMAND` (needs binding layer-shell v4; Hyprland
  advertises v5). Hyprland hands over keyboard focus once and then sends
  `leave` and never re-focuses us — the overlay goes deaf and the process
  ends. Not viable.
- Nudging the cursor while keeping the grab. No effect: the grab is what
  blocks delivery, so both parts are required.

### Bug C — zero timestamp (older Hyprland)
`VirtualPointer.cpp` historically dropped axis events with `timeMs == 0`. In
0.56.2 this check is gone, but we still send a real timestamp (`now_ms()` from
`CLOCK_MONOTONIC`) to be safe across versions.

### Setting that makes it actually scroll
`~/.config/hypr/input.lua`:
```
input { emulate_discrete_scroll = 2 }
```
With `discrete=1` → `deltaDiscrete=120` → Hyprland's emulation path emits one
notch per press. (Default `1` bypasses emulation and relies on the smooth
`axis` value, which was unreliable.)

### Verification we did have
`/tmp/wlkbptr-scroll.log` (gated by `WLKBPTR_DEBUG=1`) confirmed events were
emitted correctly (`time` non-zero, `discrete=120` in the log reflects our
`step`, coords = active window center). So the client side is correct; the
blocker was Bug B (overlay eats the axis) + Bug A (wrong discrete scale).

---

## 3. Multi-instance bug — single-instance guard (FIXED)

**Symptom reported by user**: launching `wl-kbptr` while one is already
running stacks a second full-screen overlay, and the stack could not be
dismissed (required a manual PC reset).

**Fix** — `terminate_other_instances()` in `src/main.c` (called after
`load_modes`, skipped for `--only-print`/`--version`/`--help`/`--print-default`):
1. Scans `/proc`; for any process whose `/proc/<pid>/comm` is **exactly**
   `wl-kbptr` (and `pid != self`), sends `SIGTERM`. The exact match matters:
   `comm` is newline-terminated, and a prefix match would also kill the
   `wl-kbptr-*-active-win` helper scripts.
2. Waits for each to actually exit (up to ~500ms each) so the compositor is
   not tearing down the old overlay while it maps ours.

There is deliberately **no pidfile**: `SIGTERM` never runs `atexit`, so it
could only ever go stale, and the `/proc` scan is what actually does the work.

Net effect:
- Only one overlay can exist; a new launch replaces the old.
- **Recovery**: if an overlay ever gets stuck, just launch `wl-kbptr` again
  (same keybind or terminal) and it SIGTERMs the stuck instance. No more hard
  resets.

---

## 4. RESOLVED — "overlay launches, nothing shows / cannot be closed"

**Report**: `SUPER+NTILDE` (scroll) *and* `CTRL+NTILDE` (bisect) put up an
overlay that renders **nothing**, swallows every key including `Esc`, and
cannot be dismissed by relaunching — requiring a hard reset.

### Root cause (confirmed, 2026-08-30)

`has_last_mode_returned()` in `src/mode.c` read
`state->mode_interfaces[state->current_mode]` **without checking for
`NO_MODE_ENTERED` (-1)** — an out-of-bounds read of the 8 bytes preceding the
array. This is a latent bug that exists upstream too, but it was harmless
there by pure luck: the field before `mode_interfaces` was
`struct rect result`, initialised to `{-1,-1,-1,-1}`, so the OOB read yielded
`0xFFFFFFFFFFFFFFFF` — non-NULL — and `has_last_mode_returned()` correctly
returned false.

Adding `state.scroll` (zero-initialised, §1) **between `result` and
`mode_interfaces`** changed what that OOB read returns to `NULL`. So:

1. `enter_first_mode()` → `enter_next_mode()` → `has_last_mode_returned()`
   reads `mode_interfaces[-1]` == NULL → returns **true** → returns early
   without entering any mode. `current_mode` stays at -1.
2. `send_frame()` → `mode_render()` → same OOB → returns early **without
   painting**. Result: a full-size, fully transparent buffer. Verified with
   `WAYLAND_DEBUG=1`: a 1920x1080 buffer is attached and committed, and
   `grim` shows nothing on screen.
3. The overlay holds `set_keyboard_interactivity(1)` on the OVERLAY layer, so
   it owns the keyboard — invisible **and** undismissable.

The `handle_keyboard_key` guard added earlier (`ignore keys while
current_mode == NO_MODE_ENTERED`) turned this into the hard-reset symptom by
also swallowing `Esc`.

**Fix**: `has_last_mode_returned()` returns false for `NO_MODE_ENTERED`;
`mode_handle_key()` and `mode_render()` guard the same case explicitly.
`Esc` is now handled in `handle_keyboard_key` **before** any mode dispatch, so
the overlay can never become undismissable regardless of mode state.

### Fixes that were reverted (they were misdiagnoses, not fixes)
- `handle_layer_surface_configure` painting the first frame against a guessed
  "fallback output". Upstream waits for `surface.enter`, which arrives ~15ms
  after configure here; guessing picks the wrong monitor on multi-head.
- `handle_surface_enter` resetting `current_mode = NO_MODE_ENTERED` to
  "re-enter on the real output" — this re-ran `enter` over live mode state,
  leaking it.

### Other defects fixed at the same time
- `load_home_row()` called `exit(1)` when a keycode had no UTF-8 mapping. A
  seat's keymap changes whenever any virtual-keyboard client appears (`wtype`,
  `ydotool`, on-screen keyboards), and those minimal keymaps do not carry all
  28 keys → wl-kbptr died mid-session. It now warns and keeps the previous
  mapping. A `_Static_assert` ties `key_codes[]` to `HOME_ROW_LEN_WITH_BTN`.
- `determine_division()` returned `DIVISION_25` for any area >1px, producing
  zero-width/height cells below 5px. Falls back to `DIVISION_4` under 5px.
- 5x5 label font was clamped to `label_font_size` (20px), overflowing cells
  from the third bisect step on (cells are 15x8px there). It is now scaled to
  fit the cell in both axes.
- `scroll_mode_reenter()` called `reenter_prev_mode()` (copied from
  `mode_click`, which is a pass-through mode). For an interactive mode this
  skipped straight past scroll mode. Now a no-op.
- Scroll HUD text colour was `0xffffff` — colours are **RRGGBBAA**, so that is
  cyan, not white. Now `0xffffffff`. The renderer also restores
  `CAIRO_OPERATOR_OVER` after clearing.
- `utils_wayland.c` used `lround`/`fprintf` without `<math.h>`/`<stdio.h>`;
  `scroll_pointer()` now bails out on a NULL output or empty seat list.
- The single-instance guard dropped its pidfile (SIGTERM never runs `atexit`,
  and nothing read it). The `/proc` comm scan is the part that works and is
  kept, renamed `terminate_other_instances()`.

### Verification performed (live Hyprland, `wtype` + `grim`)
- bisect 5x5 renders; 3 drill-downs + `Enter` print `15x9+954+537 +0+0 l`.
- `Esc` exits cleanly (status 0) from both bisect and scroll mode.
- Scroll HUD renders in white; `k` via the hypr helper scrolls a static `foot`
  window up by exactly 5 lines (353 -> 348).
- Second launch replaces the first (1 instance, not 2); helper scripts are
  spared by the exact `comm` match.

## 5. Things to remember
- Hyprland, not Sway → the Sway helper (`wl-kbptr-sway-active-win`) does **not**
  work here; use `wl-kbptr-hypr-active-win` (hyprctl-based).
- The binary is built **without OpenCV** (`meson setup build --buildtype=release`,
  no `-Dopencv=enabled`). Any `mode_floating.source=detect` in
  `~/.config/wl-kbptr/config` makes the **whole config fail to parse** —
  `err: Binary not build with OpenCV. 'detect' source not supported.` — and
  wl-kbptr exits before `-o` overrides are applied, so keybindings appear dead
  (no overlay). Symptoms: `timeout 5 wl-kbptr -o modes=bisect,click` prints the
  OpenCV error and exits 1. Fix: keep the config OpenCV-free (e.g. default
  `modes=bisect,click`); OpenCV-dependent settings only via a separate config
  passed with `-c`, or rebuild with `-Dopencv=enabled`.
- Scroll is a **live** mode now. The overlay's pointer capture is solved by
  dropping the exclusive grab around the axis send plus a one-pixel cursor
  nudge (§1/§2 Bug B). Keep that whole sequence to a single roundtrip.
- `discrete` for `axis_discrete` must be `1` (Hyprland ×120). Never `120`.
- `emulate_discrete_scroll = 2` in `input.lua` is required for virtual-pointer
  scroll to register.
- `state.scroll` no longer exists. Note *why* it mattered: inserting it before
  `mode_interfaces` in `struct state` is what detonated the latent
  `mode_interfaces[-1]` OOB read (§4). Adding fields there is now safe, but the
  episode is worth remembering.
- Local-only changes; not committed/pushed upstream.
