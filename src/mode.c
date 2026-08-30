// SPDX-License-Identifier: GPL-3.0-only

#include "mode.h"

#include "log.h"
#include "utils_cairo.h"
#include "utils_wayland.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

extern struct mode_interface tile_mode_interface;
extern struct mode_interface floating_mode_interface;
extern struct mode_interface bisect_mode_interface;
extern struct mode_interface split_mode_interface;
extern struct mode_interface click_mode_interface;
extern struct mode_interface scroll_mode_interface;

struct mode_interface *mode_interfaces[] = {
    &tile_mode_interface,
    &floating_mode_interface,
    &bisect_mode_interface,
    &split_mode_interface,
    &click_mode_interface,
    &scroll_mode_interface,
    NULL,
};

static struct mode_interface *find_mode_interface_by_name(char *name) {
    struct mode_interface **curr_mode_interface = mode_interfaces;
    while (*curr_mode_interface != NULL) {
        if (strcmp(name, (*curr_mode_interface)->name) == 0) {
            return *curr_mode_interface;
        }

        curr_mode_interface++;
    }

    return NULL;
}

int load_modes(struct state *state, char *modes) {
    char buf[strlen(modes) + 1];
    strcpy(buf, modes);

    int mode_i = 0;

    static const char *delim = ",";
    char              *tok   = strtok(buf, delim);
    while (tok != NULL) {
        if (mode_i >= MAX_NUM_MODES) {
            LOG_ERR("Only %d modes can be specified.", MAX_NUM_MODES);
            return 1;
        }

        struct mode_interface *mode_interface =
            find_mode_interface_by_name(tok);
        if (mode_interface == NULL) {
            LOG_ERR("Unknown mode '%s'.", tok);
            return 2;
        }

        state->mode_interfaces[mode_i++] = mode_interface;
        tok                              = strtok(NULL, delim);
    }

    if (mode_i < MAX_NUM_MODES) {
        state->mode_interfaces[mode_i] = NULL;
    }

    state->current_mode = NO_MODE_ENTERED;
    return 0;
}

/**
 * First half of a drag: press the button at the selection we just made and run
 * the whole mode chain again so the user can pick where to release it.
 *
 * The button stays down throughout that second pass, so the window underneath
 * sees a real drag as the cursor is navigated rather than a jump at the end.
 */
static void drag_press_and_restart(struct state *state, struct rect area) {
    const int32_t x = area.x + area.w / 2;
    const int32_t y = area.y + area.h / 2;

    // Whatever the first pass settled on is the button we drag with -- the
    // click keys (`n`, `m`, `,`), or a `click` mode in the chain. Only when
    // nothing chose one does `mode_drag.button` apply. Clearing it afterwards
    // matters: it is what stops the second pass's navigation keys from firing
    // clicks as they move the cursor.
    const enum click button = state->click == CLICK_NONE
                                  ? state->config.mode_drag.button
                                  : state->click;
    state->click            = CLICK_NONE;

    drag_begin(state, x, y, button);
    state->drag.phase = DRAG_PHASE_TARGET;

    for (int i = 0; i < state->current_mode && i < MAX_NUM_MODES; i++) {
        if (state->mode_interfaces[i] == NULL) {
            break;
        }

        state->mode_interfaces[i]->restart(state, state->mode_states[i]);

        // Only the mode we rewind to keeps its state. The rest are entered
        // again on the way back up, which overwrites the slot, so their
        // current state has to go now or it is lost with nothing pointing at
        // it. (Restarting them first is still worth it for anything that
        // hangs onto expensive setup, like `floating`'s detected areas.)
        if (i > 0 && state->mode_states[i] != NULL) {
            state->mode_interfaces[i]->free(state->mode_states[i]);
            state->mode_states[i] = NULL;
        }
    }

    state->current_mode = 0;
}

void enter_next_mode(struct state *state, struct rect area) {
    if (has_last_mode_returned(state)) {
        return;
    }

    state->current_mode += 1;

    if (has_last_mode_returned(state)) {
        if (state->drag.phase == DRAG_PHASE_START) {
            drag_press_and_restart(state, area);
            return;
        }

        if (state->drag.phase == DRAG_PHASE_TARGET) {
            state->click = CLICK_DRAG;
        }

        memcpy(&state->result, &area, sizeof(struct rect));
        return;
    }

    state->mode_states[state->current_mode] =
        state->mode_interfaces[state->current_mode]->enter(state, area);
}

bool has_last_mode_returned(struct state *state) {
    // No mode has been entered yet, so the last one cannot have returned.
    // Guarding here also keeps us from indexing `mode_interfaces[-1]`.
    if (state->current_mode == NO_MODE_ENTERED) {
        return false;
    }

    return state->current_mode >= MAX_NUM_MODES ||
           state->mode_interfaces[state->current_mode] == NULL;
}

bool reenter_prev_mode(struct state *state) {
    if (state->current_mode <= 0) {
        return false;
    }

    void *current_mode_state = state->mode_states[state->current_mode];
    if (current_mode_state != NULL) {
        state->mode_interfaces[state->current_mode]->free(current_mode_state);
    }

    state->current_mode--;
    state->mode_interfaces[state->current_mode]->reenter(
        state, state->mode_states[state->current_mode]
    );

    return true;
}

void free_mode_states(struct state *state) {
    if (state->current_mode == NO_MODE_ENTERED) {
        return;
    }

    for (int i = 0; i <= state->current_mode; i++) {
        if (state->mode_interfaces[i] == NULL ||
            state->mode_states[i] == NULL) {
            return;
        }

        state->mode_interfaces[i]->free(state->mode_states[i]);
    }
}

bool mode_handle_key(struct state *state, xkb_keysym_t sym, char *text) {
    if (state->current_mode == NO_MODE_ENTERED ||
        has_last_mode_returned(state)) {
        return false;
    }

    return state->mode_interfaces[state->current_mode]->key(
        state, state->mode_states[state->current_mode], sym, text
    );
}
/**
 * Mark where the button is being held down, and tie it to where the cursor is
 * now, so the second selection is made with the drag it is part of on screen.
 */
static void render_drag_origin(struct state *state, cairo_t *cairo) {
    const struct mode_drag_config *config = &state->config.mode_drag;
    const double                   x      = state->drag.start_x;
    const double                   y      = state->drag.start_y;
    const double                   size   = config->start_marker_size;
    const double                   bar_w  = fmax(1.5, size / 4.);
    const double                   bar_h  = size * 2.;

    cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);

    cairo_set_source_u32(cairo, config->line_color);
    cairo_set_line_width(cairo, 1);
    cairo_move_to(cairo, x + .5, y + .5);
    cairo_line_to(cairo, state->drag.x + .5, state->drag.y + .5);
    cairo_stroke(cairo);

    cairo_set_source_u32(cairo, config->start_marker_color);
    cairo_set_line_cap(cairo, CAIRO_LINE_CAP_SQUARE);

    switch (config->start_marker_shape) {
    case DRAG_MARKER_CIRCLE:
        cairo_arc(cairo, x, y, size, 0, 2 * M_PI);
        cairo_fill(cairo);
        break;

    case DRAG_MARKER_RECTANGLE:
        cairo_set_line_width(cairo, bar_w);
        cairo_move_to(cairo, x, y - bar_h / 2);
        cairo_line_to(cairo, x, y + bar_h / 2);
        cairo_stroke(cairo);
        break;

    case DRAG_MARKER_CARET:;
        // A text insertion cursor: a vertical bar with serifs, which is what
        // the common case -- selecting text -- wants to see.
        const double serif_w = size * .75;

        cairo_set_line_width(cairo, bar_w);
        cairo_move_to(cairo, x, y - bar_h / 2);
        cairo_line_to(cairo, x, y + bar_h / 2);
        cairo_stroke(cairo);

        cairo_move_to(cairo, x - serif_w / 2, y - bar_h / 2);
        cairo_line_to(cairo, x + serif_w / 2, y - bar_h / 2);
        cairo_stroke(cairo);

        cairo_move_to(cairo, x - serif_w / 2, y + bar_h / 2);
        cairo_line_to(cairo, x + serif_w / 2, y + bar_h / 2);
        cairo_stroke(cairo);
        break;
    }
}

/**
 * Say, in as many words, that a button is being held and what to do about it.
 *
 * Without this the second pass is easy to mistake for nothing having happened:
 * the chain restarts to the same grid it started with, and the marker can land
 * right where the pointer already was.
 */
static void render_drag_hud(struct state *state, cairo_t *cairo) {
    const char *lines[] = {
        "Holding button - pick where to release",
        "Esc cancels - releases where it was pressed",
    };
    const int num_lines = sizeof(lines) / sizeof(lines[0]);

    cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
    cairo_select_font_face(
        cairo, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL
    );
    cairo_set_font_size(cairo, 18);

    double               max_w = 0;
    cairo_text_extents_t te;
    for (int i = 0; i < num_lines; i++) {
        cairo_text_extents(cairo, lines[i], &te);
        max_w = fmax(max_w, te.width);
    }

    const int pad   = 12;
    const int line  = 22;
    const int box_w = (int)max_w + pad * 2;
    const int box_h = line * num_lines + pad * 2;
    const int x     = 20;
    const int y     = 20;

    cairo_set_source_u32(cairo, 0x111111ee);
    cairo_rectangle(cairo, x, y, box_w, box_h);
    cairo_fill(cairo);

    cairo_set_source_u32(cairo, state->config.mode_drag.start_marker_color);
    cairo_rectangle(cairo, x, y, 4, box_h);
    cairo_fill(cairo);

    cairo_set_source_u32(cairo, 0xffffffff);
    for (int i = 0; i < num_lines; i++) {
        cairo_move_to(cairo, x + pad + 6, y + pad + line * (i + 1) - 6);
        cairo_show_text(cairo, lines[i]);
    }
}

void mode_render(struct state *state, cairo_t *cairo) {
    if (state->current_mode == NO_MODE_ENTERED ||
        has_last_mode_returned(state)) {
        return;
    }

    state->mode_interfaces[state->current_mode]->render(
        state, state->mode_states[state->current_mode], cairo
    );

    if (state->drag.phase == DRAG_PHASE_TARGET) {
        render_drag_origin(state, cairo);
        render_drag_hud(state, cairo);
    }
}
