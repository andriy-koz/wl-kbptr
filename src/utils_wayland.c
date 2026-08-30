// SPDX-License-Identifier: GPL-3.0-only

#include "utils_wayland.h"

#include "state.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

// Continuous axis delta a real mouse wheel reports for one click.
#define WHEEL_NOTCH 15.0

// How long to leave the compositor alone after dropping the keyboard grab,
// before and after sending a button event, in microseconds.
//
// Unlike the axis events in `scroll_pointer()` -- which land correctly when
// the ungrab, the motion and the axis are one unflushed batch -- a button sent
// in that same batch is swallowed by our own overlay. It only reaches the
// window underneath once the ungrab has actually been processed, which needs a
// flush and a moment to settle. Measured on Hyprland 0.56.2 by middle-clicking
// into a terminal and watching for the primary-selection paste: nothing at all
// without the settle, reliable with it.
#define GRAB_SETTLE_US    40000
#define BUTTON_SETTLE_US  20000

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void _apply_transform(
    uint32_t *x, uint32_t *y, uint32_t *width, uint32_t *height,
    enum wl_output_transform transform
) {
    uint32_t temp;

    switch (transform) {
    case WL_OUTPUT_TRANSFORM_NORMAL:
        break;

    case WL_OUTPUT_TRANSFORM_90:
        temp = *x;
        *x   = *y;
        *y   = *width - temp;

        temp    = *width;
        *width  = *height;
        *height = temp;
        break;

    case WL_OUTPUT_TRANSFORM_180:
        *x = *width - *x;
        *y = *height - *y;
        break;

    case WL_OUTPUT_TRANSFORM_270:
        temp = *x;
        *x   = *height - *y;
        *y   = temp;

        temp    = *width;
        *width  = *height;
        *height = temp;
        break;

    case WL_OUTPUT_TRANSFORM_FLIPPED:
        *x = *width - *x;
        break;

    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
        *x = *width - *x;
        _apply_transform(x, y, width, height, WL_OUTPUT_TRANSFORM_90);
        break;

    case WL_OUTPUT_TRANSFORM_FLIPPED_180:
        *x = *width - *x;
        _apply_transform(x, y, width, height, WL_OUTPUT_TRANSFORM_180);
        break;

    case WL_OUTPUT_TRANSFORM_FLIPPED_270:
        *x = *width - *x;
        _apply_transform(x, y, width, height, WL_OUTPUT_TRANSFORM_270);
        break;
    }
}

/**
 * Drop or take back the layer surface's exclusive keyboard grab.
 *
 * While the grab is held, Hyprland routes *all* pointer input to our overlay
 * and ignores its empty input region, so any event we synthesise lands on
 * ourselves instead of the window underneath. Callers drop the grab around a
 * send and take it straight back, batching the whole sequence into a single
 * roundtrip so the ungrabbed window stays as short as possible -- see the
 * comment in `scroll_pointer()`.
 */
static void overlay_set_grab(struct state *state, bool grab) {
    if (state->wl_layer_surface == NULL) {
        return;
    }

    zwlr_layer_surface_v1_set_keyboard_interactivity(
        state->wl_layer_surface,
        grab ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
             : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE
    );
    wl_surface_commit(state->wl_surface);
}

/**
 * Emit a single `motion_absolute` for the given output-local position. The
 * transform is recomputed per call because `_apply_transform` rewrites the
 * width and height it is given.
 */
static void send_motion(
    struct state *state, struct zwlr_virtual_pointer_v1 *virt_pointer,
    uint32_t time, int32_t x, int32_t y
) {
    uint32_t tx = x < 0 ? 0 : (uint32_t)x;
    uint32_t ty = y < 0 ? 0 : (uint32_t)y;
    uint32_t tw = state->current_output->width;
    uint32_t th = state->current_output->height;

    _apply_transform(&tx, &ty, &tw, &th, state->current_output->transform);
    zwlr_virtual_pointer_v1_motion_absolute(virt_pointer, time, tx, ty, tw, th);
    zwlr_virtual_pointer_v1_frame(virt_pointer);
}

/**
 * Walk the cursor from (`from_x`, `from_y`) to (`to_x`, `to_y`) in `steps`
 * events rather than one teleport.
 *
 * Two reasons. A `motion_absolute` to the position the cursor already occupies
 * is a no-op for the compositor, so pointer focus is never recomputed -- the
 * one-pixel nudge below is the same fix `scroll_pointer()` needs. And toolkits
 * decide "this is a drag" from the motion they receive after a press; feeding
 * them a path rather than a jump is what a real mouse looks like.
 */
static void send_motion_path(
    struct state *state, struct zwlr_virtual_pointer_v1 *virt_pointer,
    uint32_t time, int32_t from_x, int32_t from_y, int32_t to_x, int32_t to_y,
    int steps
) {
    if (from_x == to_x && from_y == to_y) {
        send_motion(
            state, virt_pointer, time, to_x, to_y > 0 ? to_y - 1 : to_y + 1
        );
        send_motion(state, virt_pointer, time, to_x, to_y);
        return;
    }

    if (steps < 1) {
        steps = 1;
    }

    for (int i = 1; i <= steps; i++) {
        send_motion(
            state, virt_pointer, time,
            from_x + (to_x - from_x) * i / steps,
            from_y + (to_y - from_y) * i / steps
        );
    }
}

static bool pointer_usable(struct state *state) {
    if (!state->wl_virtual_pointer_mgr) {
        // We running in `--print-only` mode.
        return false;
    }

    if (state->current_output == NULL || wl_list_empty(&state->seats)) {
        return false;
    }

    return wl_display_get_error(state->wl_display) == 0;
}

static struct zwlr_virtual_pointer_v1 *create_virtual_pointer(struct state *state
) {
    return zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
        state->wl_virtual_pointer_mgr,
        ((struct seat *)state->seats.next)->wl_seat,
        state->current_output->wl_output
    );
}

void move_pointer(
    struct state *state, uint32_t x, uint32_t y, enum click click
) {
    // While a drag is in progress every cursor move has to go through the
    // pointer that holds the button down, or the window being dragged never
    // sees it.
    if (state->drag.held) {
        drag_motion(state, x, y);
        return;
    }

    if (!state->wl_virtual_pointer_mgr) {
        // We running in `--print-only` mode.
        return;
    }

    wl_display_roundtrip(state->wl_display);

    struct zwlr_virtual_pointer_v1 *virt_pointer =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
            state->wl_virtual_pointer_mgr,
            ((struct seat *)state->seats.next)->wl_seat,
            state->current_output->wl_output
        );

    uint32_t output_width  = state->current_output->width;
    uint32_t output_height = state->current_output->height;

    _apply_transform(
        &x, &y, &output_width, &output_height, state->current_output->transform
    );

    zwlr_virtual_pointer_v1_motion_absolute(
        virt_pointer, 0, x, y, output_width, output_height
    );
    zwlr_virtual_pointer_v1_frame(virt_pointer);
    wl_display_roundtrip(state->wl_display);

    // Guard on the argument, not on `state->click`: the two disagree whenever
    // a click has been chosen but this call is only meant to move the cursor,
    // which is every navigation keypress of a drag's second selection.
    if (click != CLICK_NONE && click != CLICK_DRAG) {
        int btn = 271 + click;

        zwlr_virtual_pointer_v1_button(
            virt_pointer, 0, btn, WL_POINTER_BUTTON_STATE_PRESSED
        );
        zwlr_virtual_pointer_v1_frame(virt_pointer);
        wl_display_roundtrip(state->wl_display);

        zwlr_virtual_pointer_v1_button(
            virt_pointer, 0, btn, WL_POINTER_BUTTON_STATE_RELEASED
        );
        zwlr_virtual_pointer_v1_frame(virt_pointer);
        wl_display_roundtrip(state->wl_display);
    }

    zwlr_virtual_pointer_v1_destroy(virt_pointer);
}

void scroll_pointer(
    struct state *state, uint32_t axis, int32_t notches, uint32_t x, uint32_t y
) {
    if (notches == 0) {
        return;
    }

    if (!state->wl_virtual_pointer_mgr) {
        // We running in `--print-only` mode.
        return;
    }

    if (state->current_output == NULL || wl_list_empty(&state->seats)) {
        return;
    }

    // While our layer surface holds an exclusive keyboard grab, Hyprland
    // routes *all* pointer input to it -- the empty input region is ignored --
    // so the axis would land on our own overlay instead of the window beneath.
    // Drop the grab for the duration of the send and take it straight back.
    // Everything below is a single ordered request stream, so the compositor
    // applies the release, the motion, the axis and the re-grab in sequence;
    // one roundtrip at the end is enough, which keeps the ungrabbed window as
    // short as possible.
    const bool grabbed = state->wl_layer_surface != NULL;
    if (grabbed) {
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            state->wl_layer_surface,
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE
        );
        wl_surface_commit(state->wl_surface);
    }

    struct zwlr_virtual_pointer_v1 *virt_pointer =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
            state->wl_virtual_pointer_mgr,
            ((struct seat *)state->seats.next)->wl_seat,
            state->current_output->wl_output
        );

    uint32_t output_width  = state->current_output->width;
    uint32_t output_height = state->current_output->height;

    _apply_transform(
        &x, &y, &output_width, &output_height, state->current_output->transform
    );

    const uint32_t time = now_ms();

    // Nudge the cursor to a neighbouring pixel first. Moving it to the
    // position it already occupies is a no-op for the compositor, so pointer
    // focus would never be recomputed and the axis would keep going wherever
    // it went last.
    zwlr_virtual_pointer_v1_motion_absolute(
        virt_pointer, time, x, y > 0 ? y - 1 : y + 1, output_width,
        output_height
    );
    zwlr_virtual_pointer_v1_frame(virt_pointer);

    zwlr_virtual_pointer_v1_motion_absolute(
        virt_pointer, time, x, y, output_width, output_height
    );
    zwlr_virtual_pointer_v1_frame(virt_pointer);

    // Emulate a mouse wheel, one notch per frame, so this looks exactly like
    // spinning a real wheel `notches` clicks. Sending a single event with
    // `discrete = notches` instead is honoured inconsistently: some clients
    // treat it as one click. Hyprland multiplies the discrete value by 120
    // (VirtualPointer.cpp: `deltaDiscrete = discrete * 120`), so 1 is one
    // logical notch. `WHEEL_NOTCH` is the conventional continuous delta a real
    // wheel reports per click; it is what clients use when they ignore the
    // discrete value.
    const int32_t    dir   = notches > 0 ? 1 : -1;
    const int32_t    count = notches > 0 ? notches : -notches;
    const wl_fixed_t delta = wl_fixed_from_double(dir * WHEEL_NOTCH);

    for (int32_t i = 0; i < count; i++) {
        zwlr_virtual_pointer_v1_axis_source(
            virt_pointer, WL_POINTER_AXIS_SOURCE_WHEEL
        );
        zwlr_virtual_pointer_v1_axis_discrete(
            virt_pointer, time, axis, delta, dir
        );
        zwlr_virtual_pointer_v1_axis(virt_pointer, time, axis, delta);
        zwlr_virtual_pointer_v1_frame(virt_pointer);
    }

    zwlr_virtual_pointer_v1_destroy(virt_pointer);

    if (grabbed) {
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            state->wl_layer_surface,
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
        );
        wl_surface_commit(state->wl_surface);
    }

    wl_display_roundtrip(state->wl_display);
}

static int drag_motion_steps(struct state *state) {
    int steps = (int)lround(state->config.mode_drag.motion_steps);
    return steps < 1 ? 1 : steps;
}

void drag_begin(struct state *state, int32_t x, int32_t y, enum click button) {
    state->drag.start_x = x;
    state->drag.start_y = y;
    state->drag.x       = x;
    state->drag.y       = y;

    if (state->only_print || !pointer_usable(state)) {
        return;
    }

    // One virtual pointer for the whole drag. Creating a second one for the
    // release would leave the button down: Hyprland matches a release against
    // the (button, device) pair that pressed it and silently drops the event
    // when they differ.
    state->drag.virt_pointer = create_virtual_pointer(state);
    state->drag.button       = 271 + button;

    overlay_set_grab(state, false);
    send_motion_path(
        state, state->drag.virt_pointer, now_ms(), x, y, x, y,
        drag_motion_steps(state)
    );
    wl_display_roundtrip(state->wl_display);
    usleep(GRAB_SETTLE_US);

    zwlr_virtual_pointer_v1_button(
        state->drag.virt_pointer, now_ms(), state->drag.button,
        WL_POINTER_BUTTON_STATE_PRESSED
    );
    zwlr_virtual_pointer_v1_frame(state->drag.virt_pointer);
    wl_display_roundtrip(state->wl_display);
    usleep(BUTTON_SETTLE_US);

    overlay_set_grab(state, true);
    wl_display_roundtrip(state->wl_display);

    state->drag.held = true;
}

void drag_motion(struct state *state, int32_t x, int32_t y) {
    if (!state->drag.held) {
        return;
    }

    if (!pointer_usable(state)) {
        state->drag.x = x;
        state->drag.y = y;
        return;
    }

    // The same settle as the press. Note that this is not currently enough to
    // get motion through to the window being dragged -- see `drag_end()`.
    overlay_set_grab(state, false);
    wl_display_roundtrip(state->wl_display);
    usleep(GRAB_SETTLE_US);

    send_motion_path(
        state, state->drag.virt_pointer, now_ms(), state->drag.x,
        state->drag.y, x, y, drag_motion_steps(state)
    );
    wl_display_roundtrip(state->wl_display);
    usleep(BUTTON_SETTLE_US);

    overlay_set_grab(state, true);
    wl_display_roundtrip(state->wl_display);

    state->drag.x = x;
    state->drag.y = y;
}

/**
 * Move to (`x`, `y`), release the held button there and close the drag.
 *
 * Called once the overlay is gone, so there is no grab left to drop.
 */
void drag_end(struct state *state, int32_t x, int32_t y) {
    if (!state->drag.held) {
        return;
    }

    if (pointer_usable(state)) {
        const uint32_t time = now_ms();

        send_motion_path(
            state, state->drag.virt_pointer, time, state->drag.x,
            state->drag.y, x, y, drag_motion_steps(state)
        );
        zwlr_virtual_pointer_v1_button(
            state->drag.virt_pointer, time, state->drag.button,
            WL_POINTER_BUTTON_STATE_RELEASED
        );
        zwlr_virtual_pointer_v1_frame(state->drag.virt_pointer);
        wl_display_roundtrip(state->wl_display);
    }

    state->drag.x    = x;
    state->drag.y    = y;
    state->drag.held = false;

    if (state->drag.virt_pointer != NULL) {
        zwlr_virtual_pointer_v1_destroy(state->drag.virt_pointer);
        state->drag.virt_pointer = NULL;
    }
}

/**
 * Give up on a drag that is still holding its button down -- `Esc`, a
 * `SIGTERM` from another instance starting up, a mode that failed to enter.
 *
 * The button is released back at the position it went down at rather than
 * wherever the cursor has wandered to, so an aborted drag drops what it picked
 * up where it picked it up. Leaving the button held is not an option: nothing
 * in wlroots or Hyprland synthesises a release when the pointer goes away, so
 * the seat would be stuck with a pressed button until something else clicks.
 */
void drag_abort(struct state *state) {
    if (!state->drag.held) {
        return;
    }

    drag_end(state, state->drag.start_x, state->drag.start_y);
}
