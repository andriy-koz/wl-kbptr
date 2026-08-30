// SPDX-License-Identifier: GPL-3.0-only

#include "utils_wayland.h"

#include "state.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <wayland-client.h>

// Continuous axis delta a real mouse wheel reports for one click.
#define WHEEL_NOTCH 15.0

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

void move_pointer(
    struct state *state, uint32_t x, uint32_t y, enum click click
) {
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

    if (state->click != CLICK_NONE) {
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
