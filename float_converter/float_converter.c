/*
 * Float Converter - Flipper Zero
 *
 * IEEE 754 binary32 / binary16 / bfloat16.
 *
 *   BIN  (2 rows, nibble groups; wider gap at the S|E|M boundaries)
 *   HEX  (+ format selector in the free space on the left)
 *   ---  info row: label = class (NRM/SUB/ZRO/INF/NaN),
 *        content = 2^E x significand decomposition, or NaN payload
 *   DEC  scientific notation with the minimum digits for round-trip
 *        (9 for f32, 5 for f16, 4 for bf16)
 *
 * SELECT mode: Up/Down picks the field (BIN, HEX, format, DEC),
 *              OK enters edit, Back exits the app.
 * EDIT mode  : Left/Right moves the cursor, Up/Down changes the digit,
 *              OK or Back return to SELECT.
 * Long OK (both modes): clears, with a short vibration.
 *
 * DEC edit: a decimal buffer is edited (sign, mantissa, exponent sign,
 * exponent). On every change the buffer is converted to the nearest float
 * (round to nearest, ties to even) and the other rows update. While
 * editing, DEC shows the buffer, with '~' if the float is not exactly that
 * value; on exit DEC shows the float's actual value.
 *
 * Format change: like a cast (value preserved, rounded into the new
 * format; overflow -> Inf; NaN -> quiet NaN with truncated payload).
 *
 * No libm dependency: encode/decode work on the bits of the double.
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
#define LABEL_W      20
#define BIN_ROW_BITS 16
#define NIBBLE_GAP   1
#define FIELD_GAP    3
#define HEX_GAP      2
#define FMT_X        46 /* x of the first character of the format selector */
#define FMT_CHARS    4

#define ROW_BIN  0 /* occupies rows 0 and 1 */
#define ROW_HEX  2
#define ROW_INFO 3
#define ROW_DEC  4

/* --------------------------------------------------------------- formats */

typedef enum {
    FmtF32,
    FmtF16,
    FmtBF16,
    FmtCount,
} FmtId;

typedef struct {
    const char* name; /* right-aligned to FMT_CHARS */
    uint8_t eb; /* exponent bits */
    uint8_t mb; /* mantissa (fraction) bits */
    uint8_t sig; /* significant decimal digits for round-trip */
} FmtInfo;

static const FmtInfo fmts[FmtCount] = {
    [FmtF32] = {.name = " f32", .eb = 8, .mb = 23, .sig = 9},
    [FmtF16] = {.name = " f16", .eb = 5, .mb = 10, .sig = 5},
    [FmtBF16] = {.name = "bf16", .eb = 8, .mb = 7, .sig = 4},
};

static uint8_t fmt_width(const FmtInfo* f) {
    return 1 + f->eb + f->mb;
}

static uint32_t fmt_mask(const FmtInfo* f) {
    const uint8_t w = fmt_width(f);
    return (w >= 32) ? 0xFFFFFFFFu : ((1u << w) - 1u);
}

static int fmt_bias(const FmtInfo* f) {
    return (1 << (f->eb - 1)) - 1;
}

typedef enum {
    ClsZero,
    ClsSub,
    ClsNorm,
    ClsInf,
    ClsNaN,
} FloatClass;

static uint32_t bits_exp(uint32_t bits, const FmtInfo* f) {
    return (bits >> f->mb) & ((1u << f->eb) - 1u);
}

static uint32_t bits_frac(uint32_t bits, const FmtInfo* f) {
    return bits & ((1u << f->mb) - 1u);
}

static bool bits_neg(uint32_t bits, const FmtInfo* f) {
    return (bits >> (f->eb + f->mb)) & 1u;
}

static FloatClass classify(uint32_t bits, const FmtInfo* f) {
    const uint32_t e = bits_exp(bits, f);
    const uint32_t m = bits_frac(bits, f);
    if(e == (1u << f->eb) - 1u) return m ? ClsNaN : ClsInf;
    if(e == 0) return m ? ClsSub : ClsZero;
    return ClsNorm;
}

/* ------------------------------------------------------ encode/decode */

static double double_from_bits(uint64_t b) {
    double d;
    memcpy(&d, &b, sizeof(d));
    return d;
}

static uint64_t double_to_bits(double d) {
    uint64_t b;
    memcpy(&b, &d, sizeof(b));
    return b;
}

/* Format bits -> double (exact: every value is a normal double) */
static double fdecode(uint32_t bits, const FmtInfo* f) {
    const uint64_t sign = (uint64_t)bits_neg(bits, f) << 63;
    const uint32_t e = bits_exp(bits, f);
    const uint32_t m = bits_frac(bits, f);
    const int bias = fmt_bias(f);

    if(e == (1u << f->eb) - 1u) /* Inf / NaN, payload aligned to the top */
        return double_from_bits(sign | 0x7FF0000000000000ull | ((uint64_t)m << (52 - f->mb)));
    if(e == 0) {
        if(m == 0) return double_from_bits(sign);
        /* subnormal: m * 2^(1-bias-mb), normalized */
        const int msb = 31 - __builtin_clz(m);
        const int E = (1 - bias) - f->mb + msb;
        const uint64_t man = ((uint64_t)m << (52 - msb)) & ((1ull << 52) - 1);
        return double_from_bits(sign | ((uint64_t)(E + 1023) << 52) | man);
    }
    const int E = (int)e - bias;
    return double_from_bits(sign | ((uint64_t)(E + 1023) << 52) | ((uint64_t)m << (52 - f->mb)));
}

/* double -> format bits, round to nearest ties to even */
static uint32_t fencode(double d, const FmtInfo* f) {
    const uint64_t db = double_to_bits(d);
    const uint32_t sign = (uint32_t)(db >> 63) << (f->eb + f->mb);
    const int dexp = (int)((db >> 52) & 0x7FF);
    const uint64_t dman = db & ((1ull << 52) - 1);
    const uint32_t exp_all = (1u << f->eb) - 1u;

    if(dexp == 0x7FF) {
        if(dman == 0) return sign | (exp_all << f->mb);
        /* NaN: payload truncated, made quiet */
        const uint32_t frac = (uint32_t)(dman >> (52 - f->mb)) | (1u << (f->mb - 1));
        return sign | (exp_all << f->mb) | frac;
    }
    if(dexp == 0 && dman == 0) return sign;

    uint64_t sig; /* value = sig * 2^e2 */
    int e2;
    if(dexp == 0) {
        sig = dman;
        e2 = -1022 - 52;
    } else {
        sig = dman | (1ull << 52);
        e2 = dexp - 1023 - 52;
    }

    const int bias = fmt_bias(f);
    const int emin = 1 - bias;
    const int emax = bias;
    const int E = (63 - __builtin_clzll(sig)) + e2;
    int tE = (E < emin) ? emin : E;
    const int shift = e2 - (tE - f->mb); /* q = sig * 2^shift */

    uint64_t q;
    if(shift >= 0) {
        q = sig << shift;
    } else {
        const int rs = -shift;
        if(rs >= 64) {
            q = 0;
        } else {
            q = sig >> rs;
            const uint64_t rem = sig & ((1ull << rs) - 1);
            const uint64_t half = 1ull << (rs - 1);
            if(rem > half || (rem == half && (q & 1))) q++;
        }
    }
    if(q >> (f->mb + 1)) { /* carry from rounding */
        q >>= 1;
        tE++;
    }
    if(q >= (1ull << f->mb)) {
        if(tE > emax) return sign | (exp_all << f->mb); /* overflow -> Inf */
        return sign | ((uint32_t)(tE + bias) << f->mb) | (uint32_t)(q - (1ull << f->mb));
    }
    return sign | (uint32_t)q; /* subnormal or zero */
}

/* ------------------------------------------------------- decimal */

/*
 * WARNING: the Flipper build uses -fsingle-precision-constant, so every
 * floating-point literal without a suffix (1e22, 10.0, 0.5) is a FLOAT.
 * This file contains no floating literals: double constants are built
 * from integers. 10^k is exact in double for k <= 22, and the product
 * P10[k-1] * 10 stays exact at every step.
 */
#define P10_MAX 22
static double P10[P10_MAX + 1];

static void p10_init(void) {
    if(P10[0] != 0) return;
    P10[0] = 1;
    for(int k = 1; k <= P10_MAX; k++)
        P10[k] = P10[k - 1] * 10;
}

#define D_HALF ((double)1 / 2)

static const uint32_t U10[10] = {
    1u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u, 10000000u, 100000000u, 1000000000u,
};

/* x * 10^k */
static double scale10(double x, int k) {
    p10_init();
    if(k >= 0) {
        while(k > P10_MAX) {
            x *= P10[P10_MAX];
            k -= P10_MAX;
        }
        return x * P10[k];
    }
    k = -k;
    while(k > P10_MAX) {
        x /= P10[P10_MAX];
        k -= P10_MAX;
    }
    return x / P10[k];
}

/* v > 0 finite -> v ~= digits * 10^(e10 - sig + 1), digits has 'sig' digits */
static void to_decimal(double v, uint8_t sig, uint32_t* digits, int* e10) {
    const int E2 = (int)((double_to_bits(v) >> 52) & 0x7FF) - 1023;
    int e = (E2 * 30103) / 100000;
    double m = scale10(v, -e);
    while(m >= 10) m = scale10(v, -(++e));
    while(m < 1) m = scale10(v, -(--e));

    uint32_t d = (uint32_t)(scale10(v, (sig - 1) - e) + D_HALF);
    if(d >= U10[sig]) {
        d = U10[sig - 1];
        e++;
    }
    *digits = d;
    *e10 = e;
}

typedef struct {
    bool neg;
    uint32_t mant; /* 'sig' digits, the first one is the digit before the point */
    bool eneg;
    uint8_t exp; /* 0..99 */
} DecBuf;

static double decbuf_value(const DecBuf* b, uint8_t sig) {
    const int e = (b->eneg ? -(int)b->exp : (int)b->exp) - (sig - 1);
    const double v = scale10((double)b->mant, e);
    return b->neg ? -v : v;
}

static void decbuf_from_bits(DecBuf* b, uint32_t bits, const FmtInfo* f) {
    memset(b, 0, sizeof(*b));
    b->neg = bits_neg(bits, f);
    const FloatClass c = classify(bits, f);
    if(c != ClsNorm && c != ClsSub) return; /* zero, Inf, NaN -> buffer left at 0 */

    double v = fdecode(bits, f);
    if(v < 0) v = -v;
    int e10;
    to_decimal(v, f->sig, &b->mant, &e10);
    b->eneg = e10 < 0;
    b->exp = (uint8_t)(e10 < 0 ? -e10 : e10);
}

/* -------------------------------------------------------------- strings */

static char digit_char(uint8_t d) {
    return (char)(d < 10 ? '0' + d : 'A' + (d - 10));
}

/* Appends v in decimal with at least 'pad' digits, returns the new length */
static uint8_t put_uint(char* s, uint8_t len, uint32_t v, uint8_t pad) {
    char tmp[10];
    uint8_t n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while(v || n < pad);
    while(n)
        s[len++] = tmp[--n];
    s[len] = 0;
    return len;
}

static uint8_t put_int(char* s, uint8_t len, int v) {
    if(v < 0) {
        s[len++] = '-';
        v = -v;
    }
    return put_uint(s, len, (uint32_t)v, 1);
}

/* Buffer -> "+d.dddE+ee"; map[cursor] = character index */
static uint8_t decbuf_string(const DecBuf* b, uint8_t sig, char* out, int8_t* map) {
    uint8_t i = 0;
    if(map) map[sig + 3] = (int8_t)i;
    out[i++] = b->neg ? '-' : '+';
    for(int k = sig - 1; k >= 0; k--) {
        if(map) map[3 + k] = (int8_t)i;
        out[i++] = (char)('0' + (b->mant / U10[k]) % 10);
        if(k == sig - 1) out[i++] = '.';
    }
    out[i++] = 'E';
    if(map) map[2] = (int8_t)i;
    out[i++] = b->eneg ? '-' : '+';
    if(map) map[1] = (int8_t)i;
    out[i++] = (char)('0' + b->exp / 10);
    if(map) map[0] = (int8_t)i;
    out[i++] = (char)('0' + b->exp % 10);
    out[i] = 0;
    return i;
}

/* Info row: decomposition or payload */
static const char* info_string(uint32_t bits, const FmtInfo* f, char* out) {
    static const char* labels[] = {"ZRO", "SUB", "NRM", "INF", "NaN"};
    const FloatClass c = classify(bits, f);
    const uint32_t m = bits_frac(bits, f);
    uint8_t n = 0;
    out[0] = 0;

    if(c == ClsNorm || c == ClsSub) {
        /* significand = (lead*2^mb + m) / 2^mb, printed with sig digits */
        const uint64_t lead = (c == ClsNorm) ? (1ull << f->mb) : 0;
        const int E = (c == ClsNorm) ? (int)bits_exp(bits, f) - fmt_bias(f) : 1 - fmt_bias(f);
        const uint64_t num = (lead + m) * U10[f->sig - 1];
        const uint32_t d = (uint32_t)((num + (1ull << (f->mb - 1))) >> f->mb);
        out[n++] = '2';
        out[n++] = '^';
        n = put_int(out, n, E);
        /* space only if there is room: max 17 characters right of the label */
        if(n + 2 + 1 + f->sig <= 17) out[n++] = ' ';
        out[n++] = 'x';
        n = put_uint(out, n, d / U10[f->sig - 1], 1);
        out[n++] = '.';
        n = put_uint(out, n, d % U10[f->sig - 1], f->sig - 1);
    } else if(c == ClsNaN) {
        const bool quiet = (m >> (f->mb - 1)) & 1u;
        const uint32_t payload = m & ((1u << (f->mb - 1)) - 1u);
        const char* kind = quiet ? "quiet 0x" : "sig 0x";
        while(*kind)
            out[n++] = *kind++;
        const uint8_t hd = (uint8_t)((f->mb - 1 + 3) / 4);
        for(int k = hd - 1; k >= 0; k--)
            out[n++] = digit_char((payload >> (4 * k)) & 0xF);
        out[n] = 0;
    } else if(c == ClsZero) {
        out[n++] = bits_neg(bits, f) ? '-' : '+';
        out[n++] = '0';
        out[n] = 0;
    }
    return labels[c];
}

/* ---------------------------------------------------------------- model */

typedef enum {
    FieldBin,
    FieldHex,
    FieldFmt,
    FieldDec,
    FieldCount,
} Field;

typedef enum {
    ModeSelect,
    ModeEdit,
} Mode;

typedef struct {
    FuriMutex* mutex;
    uint32_t bits; /* always masked to the format width */
    FmtId fmt;
    Field field;
    Mode mode;
    uint8_t cursor; /* index from the LSB / from the rightmost position */
    DecBuf dec; /* DEC edit buffer, valid while editing FieldDec */
    bool dec_inexact;
    bool haptic;
} AppState;

static const FmtInfo* cur_fmt(const AppState* s) {
    return &fmts[s->fmt];
}

static uint8_t field_positions(const AppState* s, Field f) {
    const FmtInfo* fi = cur_fmt(s);
    switch(f) {
    case FieldBin:
        return fmt_width(fi);
    case FieldHex:
        return fmt_width(fi) / 4;
    case FieldDec:
        return fi->sig + 4; /* exp(2) + exp sign + mantissa + sign */
    default:
        return 1;
    }
}

static uint64_t ipow(uint16_t radix, uint8_t exp) {
    uint64_t p = 1;
    while(exp--)
        p *= radix;
    return p;
}

/* Cycles digit 'pos' with wrap, skipping values > max */
static void digit_step(uint64_t* v, uint16_t radix, uint8_t pos, int dir, uint64_t max) {
    const uint64_t p = ipow(radix, pos);
    const int old = (int)((*v / p) % radix);
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

static void apply_decbuf(AppState* s) {
    const FmtInfo* f = cur_fmt(s);
    const double v = decbuf_value(&s->dec, f->sig);
    s->bits = fencode(v, f);
    s->dec_inexact = fdecode(s->bits, f) != v;
}

static void set_fmt(AppState* s, FmtId nf) {
    const double v = fdecode(s->bits, cur_fmt(s));
    s->fmt = nf;
    s->bits = fencode(v, cur_fmt(s));
}

static void edit_step(AppState* s, int dir) {
    const FmtInfo* f = cur_fmt(s);
    uint64_t v;

    switch(s->field) {
    case FieldBin:
    case FieldHex:
        v = s->bits;
        digit_step(&v, s->field == FieldBin ? 2 : 16, s->cursor, dir, fmt_mask(f));
        s->bits = (uint32_t)v;
        break;
    case FieldFmt:
        set_fmt(s, (FmtId)((s->fmt + FmtCount + dir) % FmtCount));
        break;
    case FieldDec: {
        DecBuf* b = &s->dec;
        const uint8_t c = s->cursor;
        if(c <= 1) {
            v = b->exp;
            digit_step(&v, 10, c, dir, 99);
            b->exp = (uint8_t)v;
        } else if(c == 2) {
            b->eneg = !b->eneg;
        } else if(c == f->sig + 3) {
            b->neg = !b->neg;
        } else {
            v = b->mant;
            digit_step(&v, 10, (uint8_t)(c - 3), dir, U10[f->sig] - 1);
            b->mant = (uint32_t)v;
        }
        apply_decbuf(s);
        break;
    }
    default:
        break;
    }
}

/* ----------------------------------------------------------------- draw */

static int row_top(uint8_t row) {
    return ROW_Y0 + row * ROW_H;
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

static bool editing(const AppState* s, Field f) {
    return s->field == f && s->mode == ModeEdit;
}

/* BIN: bits 31..16 on top, 15..0 below; gaps at nibbles and S|E|M boundaries */
static void draw_bin(Canvas* canvas, const AppState* s) {
    const FmtInfo* f = cur_fmt(s);
    const uint8_t w = fmt_width(f);
    int x = SCREEN_W - CHAR_W;
    for(uint8_t pos = 0; pos < w; pos++) {
        if(pos == BIN_ROW_BITS) x = SCREEN_W - CHAR_W;
        const uint8_t row = (pos < BIN_ROW_BITS) ? ROW_BIN + 1 : ROW_BIN;
        draw_char(
            canvas,
            x,
            row_top(row),
            (char)('0' + ((s->bits >> pos) & 1u)),
            editing(s, FieldBin) && pos == s->cursor);
        const uint8_t next = pos + 1;
        x -= CHAR_W;
        if(next == f->mb || next == f->mb + f->eb)
            x -= FIELD_GAP;
        else if(next % 4 == 0)
            x -= NIBBLE_GAP;
    }
}

static void draw_hex(Canvas* canvas, const AppState* s) {
    const uint8_t n = fmt_width(cur_fmt(s)) / 4;
    for(uint8_t pos = 0; pos < n; pos++) {
        const int x = SCREEN_W - (pos + 1) * CHAR_W - (pos / 4) * HEX_GAP;
        draw_char(
            canvas,
            x,
            row_top(ROW_HEX),
            digit_char((s->bits >> (4 * pos)) & 0xF),
            editing(s, FieldHex) && pos == s->cursor);
    }
}

static void draw_fmt_field(Canvas* canvas, const AppState* s) {
    const bool selected = (s->field == FieldFmt);
    const bool edit = editing(s, FieldFmt);
    const int top = row_top(ROW_HEX);
    const int fx = FMT_X - 2;
    const int fw = FMT_CHARS * CHAR_W + 3;

    if(edit) {
        const int ax = fx - 4;
        for(int i = 0; i < 3; i++) {
            canvas_draw_line(canvas, ax - i, top + 1 + i, ax + i, top + 1 + i);
            canvas_draw_line(canvas, ax - i, top + 9 - i, ax + i, top + 9 - i);
        }
    }
    if(selected) {
        canvas_draw_box(canvas, fx, top, fw, ROW_H - 1);
        canvas_set_color(canvas, ColorWhite);
    } else {
        canvas_draw_frame(canvas, fx, top, fw, ROW_H - 1);
    }
    canvas_draw_str(canvas, FMT_X, top + BASELINE_OFS, cur_fmt(s)->name);
    canvas_set_color(canvas, ColorBlack);
}

static void draw_right(Canvas* canvas, uint8_t row, const char* str) {
    const int x = SCREEN_W - (int)strlen(str) * CHAR_W;
    canvas_draw_str(canvas, x, row_top(row) + BASELINE_OFS, str);
}

static void draw_dec(Canvas* canvas, const AppState* s) {
    const FmtInfo* f = cur_fmt(s);
    const int top = row_top(ROW_DEC);
    char str[20];
    int8_t map[16];

    if(editing(s, FieldDec)) {
        const uint8_t n = decbuf_string(&s->dec, f->sig, str, map);
        const int x0 = SCREEN_W - n * CHAR_W;
        for(uint8_t i = 0; i < n; i++)
            draw_char(canvas, x0 + i * CHAR_W, top, str[i], map[s->cursor] == (int8_t)i);
        if(s->dec_inexact) canvas_draw_str(canvas, LABEL_W + 2, top + BASELINE_OFS, "~");
        return;
    }

    const FloatClass c = classify(s->bits, f);
    if(c == ClsInf) {
        draw_right(canvas, ROW_DEC, bits_neg(s->bits, f) ? "-INF" : "+INF");
    } else if(c == ClsNaN) {
        draw_right(canvas, ROW_DEC, "NaN");
    } else {
        DecBuf b;
        decbuf_from_bits(&b, s->bits, f);
        decbuf_string(&b, f->sig, str, NULL);
        draw_right(canvas, ROW_DEC, str);
    }
}

static void draw_callback(Canvas* canvas, void* ctx) {
    AppState* s = ctx;
    if(furi_mutex_acquire(s->mutex, 25) != FuriStatusOk) return;

    char info[24];

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontKeyboard);

    draw_label(canvas, ROW_BIN, 2, "BIN", s->field == FieldBin);
    draw_label(canvas, ROW_HEX, 1, "HEX", s->field == FieldHex);
    draw_label(canvas, ROW_DEC, 1, "DEC", s->field == FieldDec);

    draw_bin(canvas, s);
    draw_hex(canvas, s);
    draw_fmt_field(canvas, s);

    const char* cls = info_string(s->bits, cur_fmt(s), info);
    draw_label(canvas, ROW_INFO, 1, cls, false);
    draw_right(canvas, ROW_INFO, info);

    draw_dec(canvas, s);

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

    /* Long OK: clear in both modes */
    if(ev->key == InputKeyOk && ev->type == InputTypeLong) {
        s->bits = 0;
        memset(&s->dec, 0, sizeof(s->dec));
        s->dec_inexact = false;
        s->haptic = true;
        return true;
    }

    if(s->mode == ModeSelect) {
        switch(ev->key) {
        case InputKeyUp:
            if(is_step) s->field = (Field)((s->field + FieldCount - 1) % FieldCount);
            break;
        case InputKeyDown:
            if(is_step) s->field = (Field)((s->field + 1) % FieldCount);
            break;
        case InputKeyOk:
            if(is_short) {
                s->mode = ModeEdit;
                s->cursor = 0;
                if(s->field == FieldDec) {
                    decbuf_from_bits(&s->dec, s->bits, cur_fmt(s));
                    s->dec_inexact = false;
                    s->cursor = cur_fmt(s)->sig + 2; /* first mantissa digit */
                }
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
        case InputKeyLeft:
            if(is_step) s->cursor = (uint8_t)((s->cursor + 1) % n);
            break;
        case InputKeyRight:
            if(is_step) s->cursor = (uint8_t)((s->cursor + n - 1) % n);
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

int32_t float_converter_app(void* p) {
    UNUSED(p);

    FuriMessageQueue* queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    AppState* state = malloc(sizeof(AppState));
    memset(state, 0, sizeof(AppState));
    state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    state->fmt = FmtF32;
    state->field = FieldDec;
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
