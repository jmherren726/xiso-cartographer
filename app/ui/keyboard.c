/* The on-screen keyboard (keyboard.h). Portable: no SDL or nxdk header. */
#include "keyboard.h"

#include <string.h>

#include "theme.h"

#define LETTER_ROWS (UI_KBD_ROWS - 1) /* the rows of characters; the last row is SPECIALS */
#define BOTTOM (UI_KBD_ROWS - 1)

#define KBD_W 520
#define KBD_PAD 16
#define KEY_GAP 4
#define KEY_H 34
#define FIELD_H 40
#define TEXT_IN 10
#define CARET_W 2
#define MAX_DOTS 64 /* a password field shows at most this many, its end */

static const char dot[] = "\xE2\x80\xA2"; /* U+2022, in Noto Sans */
static const ui_color dim_screen = {0, 0, 0, 150};

/* Small letters, capitals, symbols. Between them every printable ASCII character but the
   space, which is the SPACE key and Y. */
static const char keys[3][LETTER_ROWS][UI_KBD_COLS + 1] = {
    {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm.@_"},
    {"1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM.@_"},
    {"1234567890", "!?#$%^&*()", "~`+=[]{}<>", "\\|/;:'\",-_"},
};

/* The bottom row's keys, each over columns [special_col[s], special_col[s + 1]). */
enum special { KEY_SHIFT, KEY_SYMBOLS, KEY_SPACE, KEY_DELETE, KEY_DONE, SPECIALS };
static const int special_col[SPECIALS + 1] = {0, 2, 4, 6, 8, UI_KBD_COLS};

static int special_at(int col)
{
    int s = 0;
    while (s + 1 < SPECIALS && col >= special_col[s + 1]) {
        s++;
    }
    return s;
}

void ui_kbd_open(ui_kbd *k, const char *label, char *buf, size_t cap, bool password)
{
    k->label = label;
    k->buf = buf;
    k->cap = cap;
    k->password = password;
    k->layer = UI_KBD_LOWER;
    k->row = 1;
    k->col = 0;
}

static void type_char(ui_kbd *k, char c)
{
    size_t n = strlen(k->buf);
    if (n + 1 < k->cap) {
        k->buf[n] = c;
        k->buf[n + 1] = '\0';
    }
}

/* Removes the last code point: a byte, and the UTF-8 continuation bytes before it. */
static void delete_last(ui_kbd *k)
{
    size_t n = strlen(k->buf);
    if (n == 0) {
        return;
    }
    n--;
    while (n > 0 && ((unsigned char)k->buf[n] & 0xC0U) == 0x80U) {
        n--;
    }
    k->buf[n] = '\0';
}

static void toggle(ui_kbd *k, ui_kbd_layer layer)
{
    k->layer = k->layer == layer ? UI_KBD_LOWER : layer;
}

static void move(ui_kbd *k, int dir)
{
    if (k->row != BOTTOM) {
        k->col = (k->col + dir + UI_KBD_COLS) % UI_KBD_COLS;
        return;
    }
    int s = (special_at(k->col) + dir + SPECIALS) % SPECIALS;
    k->col = special_col[s];
}

ui_kbd_result ui_kbd_input(ui_kbd *k, ui_button b)
{
    switch (b) {
    case UI_BTN_UP:
        k->row = (k->row + UI_KBD_ROWS - 1) % UI_KBD_ROWS;
        break;
    case UI_BTN_DOWN:
        k->row = (k->row + 1) % UI_KBD_ROWS;
        break;
    case UI_BTN_LEFT:
        move(k, -1);
        break;
    case UI_BTN_RIGHT:
        move(k, 1);
        break;
    case UI_BTN_X:
        delete_last(k);
        break;
    case UI_BTN_Y:
        type_char(k, ' ');
        break;
    case UI_BTN_LT:
        toggle(k, UI_KBD_UPPER);
        break;
    case UI_BTN_RT:
        toggle(k, UI_KBD_SYMBOLS);
        break;
    case UI_BTN_START:
        return UI_KBD_DONE;
    case UI_BTN_B:
        return UI_KBD_CANCEL;
    case UI_BTN_A:
        if (k->row != BOTTOM) {
            type_char(k, keys[k->layer][k->row][k->col]);
            break;
        }
        switch (special_at(k->col)) {
        case KEY_SHIFT:
            toggle(k, UI_KBD_UPPER);
            break;
        case KEY_SYMBOLS:
            toggle(k, UI_KBD_SYMBOLS);
            break;
        case KEY_SPACE:
            type_char(k, ' ');
            break;
        case KEY_DELETE:
            delete_last(k);
            break;
        default: /* KEY_DONE */
            return UI_KBD_DONE;
        }
        break;
    default:
        break;
    }
    return UI_KBD_EDITING;
}

/* The field: the end of the text that fits, then the caret. A password is drawn as dots made
   here, so its text never reaches the backend. */
static void draw_field(const ui_draw *d, const ui_kbd *k, ui_rect field)
{
    ui_panel(d, field, 6, ui_col_panel, ui_col_border);
    const char *s = k->buf;
    size_t n = strlen(s);
    char dots[MAX_DOTS * (sizeof dot - 1) + 1];
    if (k->password) {
        size_t count = 0;
        for (size_t i = 0; i < n && count < MAX_DOTS; i = ui_utf8_next(k->buf, n, i)) {
            memcpy(dots + count * (sizeof dot - 1), dot, sizeof dot - 1);
            count++;
        }
        dots[count * (sizeof dot - 1)] = '\0';
        s = dots;
        n = count * (sizeof dot - 1);
    }
    int lh = d->line_height(d->ctx, UI_FONT_BODY);
    int max_w = field.w - 2 * TEXT_IN - CARET_W - 2;
    size_t start = 0;
    while (start < n && d->text_width(d->ctx, UI_FONT_BODY, s + start, n - start) > max_w) {
        start = ui_utf8_next(s, n, start);
    }
    int x = field.x + TEXT_IN;
    int y = field.y + (field.h - lh) / 2;
    int w = n > start ? d->text(d->ctx, UI_FONT_BODY, x, y, ui_col_text, s + start, n - start) : 0;
    d->fill(d->ctx, (ui_rect){x + w + 2, y + 2, CARET_W, lh - 4}, ui_col_accent);
}

/* One key: a small panel, highlighted if it's the selected one, its label centred. */
static void draw_key(const ui_draw *d, ui_rect r, bool sel, ui_font f, ui_color c,
                     const char *label)
{
    ui_panel(d, r, 4, sel ? ui_col_highlight : ui_col_panel,
             sel ? ui_col_highlight_border : ui_col_border);
    int tw = d->text_width(d->ctx, f, label, strlen(label));
    int max_w = r.w - 4;
    int x = r.x + (r.w - (tw < max_w ? tw : max_w)) / 2;
    int y = r.y + (r.h - d->line_height(d->ctx, f)) / 2;
    (void)ui_text_fit(d, f, x, y, max_w, c, label);
}

void ui_kbd_draw(const ui_draw *d, const ui_kbd *k)
{
    d->fill(d->ctx, (ui_rect){0, 0, UI_SCREEN_W, UI_SCREEN_H}, dim_screen);

    int inner_w = KBD_W - 2 * KBD_PAD;
    int key_w = (inner_w - (UI_KBD_COLS - 1) * KEY_GAP) / UI_KBD_COLS;
    int grid_h = UI_KBD_ROWS * KEY_H + (UI_KBD_ROWS - 1) * KEY_GAP;
    int label_h = d->line_height(d->ctx, UI_FONT_LABEL);
    int hint_h = d->line_height(d->ctx, UI_FONT_SMALL);
    int h = KBD_PAD + label_h + 6 + FIELD_H + 12 + grid_h + 10 + hint_h + KBD_PAD;
    ui_rect box = {(UI_SCREEN_W - KBD_W) / 2, (UI_SCREEN_H - h) / 2, KBD_W, h};
    ui_panel(d, box, UI_CHAMFER, ui_col_panel, ui_col_border);

    int x = box.x + KBD_PAD;
    int y = box.y + KBD_PAD;
    (void)ui_text_fit(d, UI_FONT_LABEL, x, y, inner_w, ui_col_accent, k->label);
    y += label_h + 6;
    draw_field(d, k, (ui_rect){x, y, inner_w, FIELD_H});
    y += FIELD_H + 12;

    for (int r = 0; r < LETTER_ROWS; r++) {
        for (int c = 0; c < UI_KBD_COLS; c++) {
            char label[2] = {keys[k->layer][r][c], '\0'};
            ui_rect key = {x + c * (key_w + KEY_GAP), y + r * (KEY_H + KEY_GAP), key_w, KEY_H};
            draw_key(d, key, r == k->row && c == k->col, UI_FONT_BOLD, ui_col_text, label);
        }
    }
    static const char *const special_label[SPECIALS] = {"SHIFT", "#+=", "SPACE", "DELETE", "DONE"};
    int by = y + BOTTOM * (KEY_H + KEY_GAP);
    for (int s = 0; s < SPECIALS; s++) {
        int cols = special_col[s + 1] - special_col[s];
        ui_rect key = {x + special_col[s] * (key_w + KEY_GAP), by,
                       cols * key_w + (cols - 1) * KEY_GAP, KEY_H};
        bool on = (s == KEY_SHIFT && k->layer == UI_KBD_UPPER) ||
                  (s == KEY_SYMBOLS && k->layer == UI_KBD_SYMBOLS);
        const char *label =
            s == KEY_SYMBOLS && k->layer == UI_KBD_SYMBOLS ? "ABC" : special_label[s];
        draw_key(d, key, k->row == BOTTOM && special_at(k->col) == s, UI_FONT_LABEL,
                 on ? ui_col_accent : ui_col_text, label);
    }
    y += grid_h + 10;
    (void)ui_text_fit(d, UI_FONT_SMALL, x, y, inner_w, ui_col_dim,
                      "X delete   Y space   LT shift   RT symbols   Start done   B cancel");
}
