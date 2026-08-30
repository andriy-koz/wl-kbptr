// SPDX-License-Identifier: GPL-3.0-only

#ifndef __UTILS_WAYLAND_H_INCLUDED__
#define __UTILS_WAYLAND_H_INCLUDED__

#include "state.h"

void move_pointer(
    struct state *state, uint32_t x, uint32_t y, enum click click
);

/**
 * Scroll at (x, y) by `notches` mouse-wheel clicks. Negative scrolls up/left.
 */
void scroll_pointer(
    struct state *state, uint32_t axis, int32_t notches, uint32_t x, uint32_t y
);

/**
 * Press `button` at (x, y) and hold it. The pointer keeps holding it -- and
 * follows every later `move_pointer()` -- until `drag_end()` or `drag_abort()`
 * is called.
 */
void drag_begin(struct state *state, int32_t x, int32_t y, enum click button);

/**
 * Drag the held button to (x, y). No-op when no button is held.
 */
void drag_motion(struct state *state, int32_t x, int32_t y);

/**
 * Move to (x, y) and release the held button there.
 */
void drag_end(struct state *state, int32_t x, int32_t y);

/**
 * Release a held button back at the position it was pressed at. Safe to call
 * when nothing is held.
 */
void drag_abort(struct state *state);

#endif
