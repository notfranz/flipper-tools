/*
 * Base Converter - Flipper Zero
 *
 * Unsigned 32-bit value, shown as BIN (2 rows x 16 bits), OCT, HEX, DEC
 * and ASCII (4 bytes MSB->LSB, in the free field left of the HEX row).
 *
 * SELECT mode: Up/Down picks the base, OK enters edit, Back exits the app.
 * EDIT mode  : Left/Right moves the cursor, Up/Down changes the digit,
 *              OK or Back return to SELECT.
 *
 * Digits that would push the value past UINT32_MAX (MSB of OCT and DEC)
 * are skipped while cycling, so the value always stays valid.
 * In ASCII, Up/Down cycle only the printable characters 0x20..0x7E;
 * non-printable bytes are shown as an empty box.
 */

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- layout */

#define SCREEN_W     128
#define ROW_H        12   /* 5 rows x 12 px = 60 px */
#define ROW_Y0       2    /* vertical offset of the first row */
#define BASELINE_OFS 9    /* baseline relative to the row top */
#define CHAR_W       6    /* FontKeyboard (profont11) is monospace, 6 px */
#define GROUP_GAP    2    /* extra px between digit groups */
#define LABEL_W      20   /* label area width */
#define BIN_ROW_BITS 16
#define ASC_X        46   /* x of the first ASCII character (MSB byte) */
#define ASCII_MIN    0x20
#define ASCII_MAX    0x7E

/* ---------------------------------------------------------------- model */

typedef enum {
    BaseBin,
    BaseOct,
    BaseHex,
    BaseAsc,
    BaseDec,
    BaseCount,
} Base;

typedef struct {
    uint16_t radix;
    uint8_t digits; /* displayed digits */
    uint8_t group;  /* visual grouping */
    uint8_t row;    /* first row on screen */
    uint8_t rows;   /* rows occupied */
    const char* label;
} BaseInfo;

/* The enum order is the navigation order; 'row' is the on-screen position. */
static const BaseInfo bases[BaseCount] = {
    [BaseBin] = {.radix = 2, .digits = 32, .group = 4, .row = 0, .rows = 2, .label = "BIN"},
    [BaseOct] = {.radix = 8, .digits = 11, .group = 3, .row = 2, .rows = 1, .label = "OCT"},
    [BaseHex] = {.radix = 16, .digits = 8, .group = 4, .row = 3, .rows = 1, .label = "HEX"},
    [BaseAsc] = {.radix = 256, .digits = 4, .group = 4, .row = 3, .rows = 1, .label = NULL},
    [BaseDec] = {.radix = 10, .digits = 10, .group = 3, .row = 4, .rows = 1, .label = "DEC"},
};

typedef enum {
    ModeSelect,
    ModeEdit,
} Mode;

typedef struct {
    FuriMutex* mutex;
    uint32_t value;
    Base base;
    Mode mode;
    uint8_t cursor; /* digit index from the LSB: 0 = least significant */
} AppState;

/* ---------------------------------------------------------------- logic */

static uint64_t ipow(uint32_t radix, uint8_t exp) {
    uint64_t p = 1;
    while(exp--)
        p *= radix;
    return p;
}

static uint8_t digit_get(uint32_t value, uint16_t radix, uint8_t pos) {
    return (uint8_t)(((uint64_t)value / ipow(radix, pos)) % radix);
}

/* Increments/decrements (dir = +1/-1) digit 'pos' with wrap-around,
 * skipping values that overflow 32 bits. Digit value 0 is always
 * valid, so the loop always terminates. */
static void digit_step(uint32_t* value, uint16_t radix, uint8_t pos, int dir) {
    const uint64_t p = ipow(radix, pos);
    const int old = digit_get(*value, radix, pos);
    const uint64_t base_val = (uint64_t)*value - (uint64_t)old * p;

    for(int i = 1; i <= radix; i++) {
        int nd = (old + dir * i) % radix;
        if(nd < 0) nd += radix;
        uint64_t nv = base_val + (uint64_t)nd * p;
        if(nv <= UINT32_MAX) {
            *value = (uint32_t)nv;
            return;
        }
    }
}

static uint8_t byte_get(uint32_t value, uint8_t pos) {
    return (uint8_t)(value >> (pos * 8));
}

static bool is_printable(uint8_t c) {
    return c >= ASCII_MIN && c <= ASCII_MAX;
}

/* Cycles byte 'pos' over printable characters only; from a non-printable
 * Up goes to ' ' and Down goes to '~'. */
static void ascii_step(uint32_t* value, uint8_t pos, int dir) {
    const uint8_t shift = pos * 8;
    int c = byte_get(*value, pos);

    if(!is_printable((uint8_t)c)) {
        c = (dir > 0) ? ASCII_MIN : ASCII_MAX;
    } else {
        c += dir;
        if(c > ASCII_MAX) c = ASCII_MIN;
        if(c < ASCII_MIN) c = ASCII_MAX;
    }
    *value = (*value & ~(0xFFu << shift)) | ((uint32_t)c << shift);
}

static char digit_char(uint8_t d) {
    return (char)(d < 10 ? '0' + d : 'A' + (d - 10));
}

/* ----------------------------------------------------------------- draw */

static int row_top(uint8_t row) {
    return ROW_Y0 + row * ROW_H;
}

/* x of the digit with local index 'lpos' (0 = rightmost), right-aligned */
static int digit_x(uint8_t lpos, uint8_t group) {
    return SCREEN_W - (lpos + 1) * CHAR_W - (lpos / group) * GROUP_GAP;
}

static void draw_base(Canvas* canvas, const AppState* s, Base b) {
    const BaseInfo* bi = &bases[b];
    const bool selected = (s->base == b);
    const bool editing = selected && (s->mode == ModeEdit);
    const int top = row_top(bi->row);
    char buf[2] = {0, 0};

    /* Label: inverted when the base is selected (covers all of its rows) */
    if(selected) {
        canvas_draw_box(canvas, 0, top, LABEL_W, bi->rows * ROW_H - 1);
        canvas_set_color(canvas, ColorWhite);
    }
    const int label_y = top + (bi->rows * ROW_H) / 2 + 3;
    canvas_draw_str(canvas, 1, label_y, bi->label);
    canvas_set_color(canvas, ColorBlack);

    for(uint8_t pos = 0; pos < bi->digits; pos++) {
        uint8_t row = bi->row;
        uint8_t lpos = pos;

        /* BIN: bits 31..16 on the first row, 15..0 on the second */
        if(b == BaseBin) {
            row = bi->row + (pos < BIN_ROW_BITS ? 1 : 0);
            lpos = pos % BIN_ROW_BITS;
        }

        const int x = digit_x(lpos, bi->group);
        const int y = row_top(row);
        buf[0] = digit_char(digit_get(s->value, bi->radix, pos));

        if(editing && pos == s->cursor) {
            canvas_draw_box(canvas, x - 1, y, CHAR_W + 1, ROW_H - 1);
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_str(canvas, x, y + BASELINE_OFS, buf);
            canvas_set_color(canvas, ColorBlack);
        } else {
            canvas_draw_str(canvas, x, y + BASELINE_OFS, buf);
        }
    }
}

/* ASCII field: box left of the HEX digits, MSB byte on the left */
static void draw_ascii(Canvas* canvas, const AppState* s) {
    const BaseInfo* bi = &bases[BaseAsc];
    const bool selected = (s->base == BaseAsc);
    const bool editing = selected && (s->mode == ModeEdit);
    const bool filled = selected && !editing;
    const Color fg = filled ? ColorWhite : ColorBlack;
    const int top = row_top(bi->row);
    const int fx = ASC_X - 2;
    const int fw = bi->digits * CHAR_W + 3;
    char buf[2] = {0, 0};

    if(filled)
        canvas_draw_box(canvas, fx, top, fw, ROW_H - 1);
    else
        canvas_draw_frame(canvas, fx, top, fw, ROW_H - 1);

    for(uint8_t pos = 0; pos < bi->digits; pos++) {
        const int x = ASC_X + (bi->digits - 1 - pos) * CHAR_W;
        const uint8_t c = byte_get(s->value, pos);
        const bool cur = editing && (pos == s->cursor);

        if(cur) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, x - 1, top + 1, CHAR_W + 1, ROW_H - 3);
            canvas_set_color(canvas, ColorWhite);
        } else {
            canvas_set_color(canvas, fg);
        }

        if(is_printable(c)) {
            buf[0] = (char)c;
            canvas_draw_str(canvas, x, top + BASELINE_OFS, buf);
        } else {
            canvas_draw_frame(canvas, x, top + 3, 5, 5);
        }
    }
    canvas_set_color(canvas, ColorBlack);
}

static void draw_callback(Canvas* canvas, void* ctx) {
    AppState* s = ctx;
    if(furi_mutex_acquire(s->mutex, 25) != FuriStatusOk) return;

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontKeyboard);

    for(Base b = 0; b < BaseCount; b++) {
        if(b == BaseAsc)
            draw_ascii(canvas, s);
        else
            draw_base(canvas, s, b);
    }

    furi_mutex_release(s->mutex);
}

/* ---------------------------------------------------------------- input */

static void input_callback(InputEvent* event, void* ctx) {
    FuriMessageQueue* queue = ctx;
    furi_message_queue_put(queue, event, FuriWaitForever);
}

/* Returns false if the app must exit */
static bool handle_input(AppState* s, const InputEvent* ev) {
    const bool is_short = (ev->type == InputTypeShort);
    const bool is_step = is_short || (ev->type == InputTypeRepeat);
    const BaseInfo* bi = &bases[s->base];

    if(s->mode == ModeSelect) {
        switch(ev->key) {
        case InputKeyUp:
            if(is_step) s->base = (s->base + BaseCount - 1) % BaseCount;
            break;
        case InputKeyDown:
            if(is_step) s->base = (s->base + 1) % BaseCount;
            break;
        case InputKeyOk:
            if(is_short) {
                s->mode = ModeEdit;
                s->cursor = 0;
            }
            break;
        case InputKeyBack:
            if(is_short) return false;
            break;
        default:
            break;
        }
    } else {
        switch(ev->key) {
        case InputKeyUp:
        case InputKeyDown:
            if(is_step) {
                const int dir = (ev->key == InputKeyUp) ? +1 : -1;
                if(s->base == BaseAsc)
                    ascii_step(&s->value, s->cursor, dir);
                else
                    digit_step(&s->value, bi->radix, s->cursor, dir);
            }
            break;
        case InputKeyLeft: /* towards MSB */
            if(is_step) s->cursor = (s->cursor + 1) % bi->digits;
            break;
        case InputKeyRight: /* towards LSB */
            if(is_step) s->cursor = (s->cursor + bi->digits - 1) % bi->digits;
            break;
        case InputKeyOk:
        case InputKeyBack:
            if(is_short) s->mode = ModeSelect;
            break;
        default:
            break;
        }
    }
    return true;
}

/* ----------------------------------------------------------------- main */

int32_t base_converter_app(void* p) {
    UNUSED(p);

    FuriMessageQueue* queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    AppState* state = malloc(sizeof(AppState));
    memset(state, 0, sizeof(AppState));
    state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    state->base = BaseBin;
    state->mode = ModeSelect;

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, draw_callback, state);
    view_port_input_callback_set(view_port, input_callback, queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    InputEvent ev;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(queue, &ev, FuriWaitForever) != FuriStatusOk) continue;

        furi_mutex_acquire(state->mutex, FuriWaitForever);
        running = handle_input(state, &ev);
        furi_mutex_release(state->mutex);

        view_port_update(view_port);
    }

    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(view_port);

    furi_message_queue_free(queue);
    furi_mutex_free(state->mutex);
    free(state);

    return 0;
}
