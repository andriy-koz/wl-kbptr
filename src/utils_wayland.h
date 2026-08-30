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

#endif
