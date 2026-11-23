/*
 * Signed Converter - Flipper Zero
 *
 * 8/16/32-bit two's complement word, shown as:
 *   BIN  (2 rows: bits 31..16 / 15..0, LSB right-aligned)
 *   HEX  (+ width selector in the free space on the left)
 *   UNS  unsigned decimal
 *   SGN  signed decimal (two's complement)
 *
 * SELECT mode: Up/Down picks the field, OK enters edit, Back exits the app.
 * EDIT mode  : Left/Right moves the cursor, Up/Down changes the digit,
 *              OK or Back return to SELECT.
 *              In SGN the leftmost position is the sign (Up/Down = toggle sign).
 *              In the width selector Up/Down cycles 8 -> 16 -> 32.
 * Long OK (both modes): clears the value, with a short vibration.
 *
 * Width change: the signed value is recomputed in the new width,
 * i.e. sign extension when widening and truncation when narrowing.
 * Sign change on the minimum (e.g. -128 at 8 bit): unchanged, like NEG in hardware.
 * Digits that would take the value out of range are skipped while cycling.
 */

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- layout */

#define SCREEN_W     128
#define ROW_H        12
#define ROW_Y0       2
#define BASELINE_OFS 9
#define CHAR_W       6 /* FontKeyboard (profont11), monospace */
#define GROUP_GAP    2
#define LABEL_W      20
#define BIN_ROW_BITS 16
#define FIELD_X      46 /* x of the first character of the width selector */

#define ROW_BIN 0 /* occupies rows 0 and 1 */
#define ROW_HEX 2
#define ROW_UNS 3
#define ROW_SGN 4

/* ---------------------------------------------------------------- model */

typedef enum {
    FieldBin,
    FieldHex,
    FieldWidth,
    FieldUns,
    FieldSgn,
    FieldCount,
} Field;

typedef enum {
    ModeSelect,
    ModeEdit,
} Mode;

#define WIDTH_COUNT 3
static const uint8_t widths[WIDTH_COUNT] = {8, 16, 32};
static const uint8_t dec_digits[WIDTH_COUNT] = {3, 5, 10}; /* 255/128, 65535/32768, 2^32-1/2^31 */

typedef struct {
    FuriMutex* mutex;
    uint32_t bits; /* always masked to the current width */
    uint8_t width_idx;
    Field field;
    Mode mode;
    uint8_t cursor; /* index from the LSB; in SGN, index dec_digits is the sign */
    bool haptic;
} AppState;

/* ---------------------------------------------------------------- logic */

static uint8_t cur_width(const AppState* s) {
    return widths[s->width_idx];
}

static uint32_t mask_w(uint8_t w) {
    return (w >= 32) ? 0xFFFFFFFFu : ((1u << w) - 1u);
}

static int64_t to_signed(uint32_t bits, uint8_t w) {
    const uint64_t sign = 1ull << (w - 1);
    return (bits & sign) ? (int64_t)bits - (int64_t)(1ull << w) : (int64_t)bits;
}

static uint32_t from_signed(int64_t v, uint8_t w) {
    return (uint32_t)((uint64_t)v & mask_w(w));
}

static uint64_t ipow(uint16_t radix, uint8_t exp) {
    uint64_t p = 1;
    while(exp--)
        p *= radix;
    return p;
}

static uint8_t digit_of(uint64_t v, uint16_t radix, uint8_t pos) {
    return (uint8_t)((v / ipow(radix, pos)) % radix);
}

/* Cycles digit 'pos' (dir = +1/-1) with wrap, skipping values > max.
 * Digit value 0 is always valid, so the loop always terminates. */
static void digit_step(uint64_t* v, uint16_t radix, uint8_t pos, int dir, uint64_t max) {
    const uint64_t p = ipow(radix, pos);
    const int old = digit_of(*v, radix, pos);
    const uint64_t base_val = *v - (uint64_t)old * p;

    for(int i = 1; i <= radix; i++) {
        int nd = (old + dir * i) % radix;
        if(nd < 0) nd += radix;
        const uint64_t nv = base_val + (uint64_t)nd * p;
        if(nv <= max) {
            *v = nv;
            return;
        }
    }
}

/* Number of editable positions of the field (cursor 0..n-1) */
static uint8_t field_positions(const AppState* s, Field f) {
    switch(f) {
    case FieldBin:
        return cur_width(s);
    case FieldHex:
        return cur_width(s) / 4;
    case FieldWidth:
        return 1;
    case FieldUns:
        return dec_digits[s->width_idx];
    case FieldSgn:
        return dec_digits[s->width_idx] + 1; /* + sign */
    default:
        return 1;
    }
}

static void set_width(AppState* s, uint8_t new_idx) {
    const int64_t sv = to_signed(s->bits, cur_width(s));
    s->width_idx = new_idx;
    s->bits = from_signed(sv, cur_width(s));
}

static void edit_step(AppState* s, int dir) {
    const uint8_t w = cur_width(s);
    const uint32_t m = mask_w(w);
    uint64_t v;

    switch(s->field) {
    case FieldBin:
    case FieldHex:
    case FieldUns: {
        const uint16_t radix = (s->field == FieldBin) ? 2 : (s->field == FieldHex) ? 16 : 10;
        v = s->bits;
        digit_step(&v, radix, s->cursor, dir, m);
        s->bits = (uint32_t)v;
        break;
    }
    case FieldWidth:
        set_width(s, (uint8_t)((s->width_idx + WIDTH_COUNT + dir) % WIDTH_COUNT));
        break;
    case FieldSgn: {
        int64_t sv = to_signed(s->bits, w);
        const int64_t smin = -(int64_t)(1ull << (w - 1));
        if(s->cursor == dec_digits[s->width_idx]) {
            /* sign position: negation, the minimum stays unchanged */
            if(sv != smin) sv = -sv;
        } else {
            const bool neg = sv < 0;
            uint64_t mag = (uint64_t)(neg ? -sv : sv);
            const uint64_t max = neg ? (1ull << (w - 1)) : (1ull << (w - 1)) - 1;
            digit_step(&mag, 10, s->cursor, dir, max);
            sv = neg ? -(int64_t)mag : (int64_t)mag;
        }
        s->bits = from_signed(sv, w);
        break;
    }
    default:
        break;
    }
}

static char digit_char(uint8_t d) {
    return (char)(d < 10 ? '0' + d : 'A' + (d - 10));
}

/* ----------------------------------------------------------------- draw */

static int row_top(uint8_t row) {
    return ROW_Y0 + row * ROW_H;
}

static int digit_x(uint8_t lpos, uint8_t group) {
    return SCREEN_W - (lpos + 1) * CHAR_W - (lpos / group) * GROUP_GAP;
}

static void draw_char(Canvas* canvas, int x, int top, char c, bool inverted) {
    char buf[2] = {c, 0};
    if(inverted) {
        canvas_draw_box(canvas, x - 1, top, CHAR_W + 1, ROW_H - 1);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str(canvas, x, top + BASELINE_OFS, buf);
        canvas_set_color(canvas, ColorBlack);
    } else {
        canvas_draw_str(canvas, x, top + BASELINE_OFS, buf);
    }
}

static void draw_label(Canvas* canvas, uint8_t row, uint8_t rows, const char* label, bool sel) {
    const int top = row_top(row);
    if(sel) {
        canvas_draw_box(canvas, 0, top, LABEL_W, rows * ROW_H - 1);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_draw_str(canvas, 1, top + (rows * ROW_H) / 2 + 3, label);
    canvas_set_color(canvas, ColorBlack);
}

/* Draws 'n' digits of v in base radix, right-aligned on the row */
static void draw_number(
    Canvas* canvas,
    const AppState* s,
    Field f,
    uint64_t v,
    uint16_t radix,
    uint8_t n,
    uint8_t group,
    uint8_t row) {
    const bool editing = (s->field == f) && (s->mode == ModeEdit);
    for(uint8_t pos = 0; pos < n; pos++) {
        uint8_t r = row;
        uint8_t lpos = pos;
        if(f == FieldBin) {
            r = (pos < BIN_ROW_BITS) ? ROW_BIN + 1 : ROW_BIN;
            lpos = pos % BIN_ROW_BITS;
        }
        draw_char(
            canvas,
            digit_x(lpos, group),
            row_top(r),
            digit_char(digit_of(v, radix, pos)),
            editing && pos == s->cursor);
    }
}

static void draw_width_field(Canvas* canvas, const AppState* s) {
    const bool selected = (s->field == FieldWidth);
    const bool editing = selected && (s->mode == ModeEdit);
    const int top = row_top(ROW_HEX);
    const int fx = FIELD_X - 2;
    const int fw = 3 * CHAR_W + 3;
    static const char* labels[WIDTH_COUNT] = {" 8b", "16b", "32b"};

    /* not selected: frame; selected: filled; edit: filled + up/down arrows */
    if(editing) {
        const int ax = fx - 4; /* arrow center */
        for(int i = 0; i < 3; i++) {
            canvas_draw_line(canvas, ax - i, top + 1 + i, ax + i, top + 1 + i); /* up */
            canvas_draw_line(canvas, ax - i, top + 9 - i, ax + i, top + 9 - i); /* down */
        }
    }
    if(selected) {
        canvas_draw_box(canvas, fx, top, fw, ROW_H - 1);
        canvas_set_color(canvas, ColorWhite);
    } else {
        canvas_draw_frame(canvas, fx, top, fw, ROW_H - 1);
    }
    canvas_draw_str(canvas, FIELD_X, top + BASELINE_OFS, labels[s->width_idx]);
    canvas_set_color(canvas, ColorBlack);
}

static void draw_callback(Canvas* canvas, void* ctx) {
    AppState* s = ctx;
    if(furi_mutex_acquire(s->mutex, 25) != FuriStatusOk) return;

    const uint8_t w = cur_width(s);
    const uint8_t nd = dec_digits[s->width_idx];

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontKeyboard);

    draw_label(canvas, ROW_BIN, 2, "BIN", s->field == FieldBin);
    draw_label(canvas, ROW_HEX, 1, "HEX", s->field == FieldHex);
    draw_label(canvas, ROW_UNS, 1, "UNS", s->field == FieldUns);
    draw_label(canvas, ROW_SGN, 1, "SGN", s->field == FieldSgn);

    draw_number(canvas, s, FieldBin, s->bits, 2, w, 4, ROW_BIN);
    draw_number(canvas, s, FieldHex, s->bits, 16, w / 4, 4, ROW_HEX);
    draw_width_field(canvas, s);
    draw_number(canvas, s, FieldUns, s->bits, 10, nd, 3, ROW_UNS);

    /* SGN: magnitude + sign to the left of the most significant digit */
    const int64_t sv = to_signed(s->bits, w);
    const uint64_t mag = (uint64_t)(sv < 0 ? -sv : sv);
    draw_number(canvas, s, FieldSgn, mag, 10, nd, 3, ROW_SGN);
    draw_char(
        canvas,
        digit_x(nd - 1, 3) - CHAR_W - GROUP_GAP,
        row_top(ROW_SGN),
        sv < 0 ? '-' : '+',
        s->field == FieldSgn && s->mode == ModeEdit && s->cursor == nd);

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

    /* Long OK: clear in both modes (does not generate Short on release) */
    if(ev->key == InputKeyOk && ev->type == InputTypeLong) {
        s->bits = 0;
        s->haptic = true;
        return true;
    }

    if(s->mode == ModeSelect) {
        switch(ev->key) {
        case InputKeyUp:
            if(is_step) s->field = (s->field + FieldCount - 1) % FieldCount;
            break;
        case InputKeyDown:
            if(is_step) s->field = (s->field + 1) % FieldCount;
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
        const uint8_t n = field_positions(s, s->field);
        switch(ev->key) {
        case InputKeyUp:
            if(is_step) edit_step(s, +1);
            break;
        case InputKeyDown:
            if(is_step) edit_step(s, -1);
            break;
        case InputKeyLeft: /* towards MSB */
            if(is_step) s->cursor = (s->cursor + 1) % n;
            break;
        case InputKeyRight: /* towards LSB */
            if(is_step) s->cursor = (s->cursor + n - 1) % n;
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

int32_t signed_converter_app(void* p) {
    UNUSED(p);

    FuriMessageQueue* queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    AppState* state = malloc(sizeof(AppState));
    memset(state, 0, sizeof(AppState));
    state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    state->width_idx = 0; /* 8 bit */
    state->field = FieldSgn;
    state->mode = ModeSelect;

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, draw_callback, state);
    view_port_input_callback_set(view_port, input_callback, queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);
    NotificationApp* notif = furi_record_open(RECORD_NOTIFICATION);

    InputEvent ev;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(queue, &ev, FuriWaitForever) != FuriStatusOk) continue;

        furi_mutex_acquire(state->mutex, FuriWaitForever);
        running = handle_input(state, &ev);
        const bool haptic = state->haptic;
        state->haptic = false;
        furi_mutex_release(state->mutex);

        view_port_update(view_port);
        if(haptic) notification_message(notif, &sequence_single_vibro);
    }

    furi_record_close(RECORD_NOTIFICATION);
    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(view_port);

    furi_message_queue_free(queue);
    furi_mutex_free(state->mutex);
    free(state);

    return 0;
}
