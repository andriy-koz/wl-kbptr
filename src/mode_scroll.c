// SPDX-License-Identifier: GPL-3.0-only

#include "config.h"
#include "mode.h"
#include "state.h"
#include "utils_cairo.h"
#include "utils_wayland.h"

#include <cairo.h>
#include <math.h>
#include <stdlib.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

struct scroll_mode_state {
    struct rect area;
};

static void *scroll_mode_enter(struct state *state, struct rect area) {
    struct scroll_mode_state *ms = malloc(sizeof(*ms));
    ms->area                     = area;
    return ms;
}

static void scroll_mode_reenter(struct state *state, void *mode_state) {
    // Interactive mode: coming back here means staying here, so nothing to do.
}

static bool scroll_mode_key(
    struct state *state, void *mode_state, xkb_keysym_t keysym, char *text
) {
    struct scroll_mode_state *ms = mode_state;

    // `step` is the number of wheel notches sent per keypress.
    int32_t step = (int32_t)lround(state->config.mode_scroll.step);
    if (step < 1) {
        step = 1;
    }

    const uint32_t x = ms->area.x + ms->area.w / 2;
    const uint32_t y = ms->area.y + ms->area.h / 2;

    switch (keysym) {
    case XKB_KEY_Escape:
        state->running = false;
        break;

    case XKB_KEY_Return:
    case XKB_KEY_space:
        enter_next_mode(state, ms->area);
        return true;

    case XKB_KEY_BackSpace:
        if (!reenter_prev_mode(state)) {
            state->running = false;
        }
        return true;

    case XKB_KEY_j:
    case XKB_KEY_J:
        scroll_pointer(state, WL_POINTER_AXIS_VERTICAL_SCROLL, step, x, y);
        return true;

    case XKB_KEY_k:
    case XKB_KEY_K:
        scroll_pointer(state, WL_POINTER_AXIS_VERTICAL_SCROLL, -step, x, y);
        return true;

    case XKB_KEY_h:
    case XKB_KEY_H:
        scroll_pointer(state, WL_POINTER_AXIS_HORIZONTAL_SCROLL, -step, x, y);
        return true;

    case XKB_KEY_l:
    case XKB_KEY_L:
        scroll_pointer(state, WL_POINTER_AXIS_HORIZONTAL_SCROLL, step, x, y);
        return true;

    default:
        return false;
    }

    return false;
}

static void
scroll_mode_render(struct state *state, void *mode_state, cairo_t *cairo) {
    // Clear to fully transparent so the underlying content stays visible.
    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_u32(cairo, 0x00000000);
    cairo_paint(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);

    const char *lines[] = {
        "Scroll mode  (J↓ K↑  H← L→)",
        "Esc quit · BackSpace back · Enter next",
    };

    cairo_select_font_face(
        cairo, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL
    );
    cairo_set_font_size(cairo, 18);

    double               max_w = 0;
    cairo_text_extents_t te;
    for (int i = 0; i < 2; i++) {
        cairo_text_extents(cairo, lines[i], &te);
        max_w = fmax(max_w, te.width);
    }

    const int pad   = 12;
    const int line  = 18;
    const int box_w = (int)max_w + pad * 2;
    const int box_h = line * 2 + pad * 2 + 4;
    const int x     = 20;
    const int y     = 20;

    cairo_set_source_u32(cairo, 0x111111ee);
    cairo_rectangle(cairo, x, y, box_w, box_h);
    cairo_fill(cairo);

    cairo_set_source_u32(cairo, 0xffffffff);
    for (int i = 0; i < 2; i++) {
        cairo_move_to(cairo, x + pad, y + pad + line * (i + 1) - 4);
        cairo_show_text(cairo, lines[i]);
    }
}

static void scroll_mode_free(void *mode_state) {
    free(mode_state);
}

struct mode_interface scroll_mode_interface = {
    .name    = "scroll",
    .enter   = scroll_mode_enter,
    .reenter = scroll_mode_reenter,
    .key     = scroll_mode_key,
    .render  = scroll_mode_render,
    .free    = scroll_mode_free,
};
