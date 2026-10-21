/* Text helpers on the drawing interface (ui.h). Portable: no SDL or nxdk header. */
#include "ui.h"

#include <string.h>

ui_rect ui_safe_area(void)
{
    ui_rect r = {UI_SCREEN_W / 20, UI_SCREEN_H / 20, UI_SCREEN_W - UI_SCREEN_W / 10,
                 UI_SCREEN_H - UI_SCREEN_H / 10};
    return r;
}

size_t ui_utf8_next(const char *s, size_t n, size_t i)
{
    unsigned char c = (unsigned char)s[i];
    size_t len = c < 0x80U              ? 1
                 : (c & 0xE0U) == 0xC0U ? 2
                 : (c & 0xF0U) == 0xE0U ? 3
                 : (c & 0xF8U) == 0xF0U ? 4
                                        : 1;
    if (len > n - i) {
        return i + 1;
    }
    for (size_t k = 1; k < len; k++) {
        if (((unsigned char)s[i + k] & 0xC0U) != 0x80U) {
            return i + 1;
        }
    }
    return i + len;
}

size_t ui_fit(const ui_draw *d, ui_font f, const char *s, size_t n, int max_w)
{
    if (max_w <= 0) {
        return 0;
    }
    if (d->text_width(d->ctx, f, s, n) <= max_w) {
        return n;
    }
    /* Widths grow with the prefix, so find the longest prefix that fits by bisection over
       byte positions, then step back to a code point boundary. */
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo + 1) / 2;
        if (d->text_width(d->ctx, f, s, mid) <= max_w) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    size_t fit = 0;
    for (size_t i = 0; i < n;) {
        size_t next = ui_utf8_next(s, n, i);
        if (next > lo) {
            break;
        }
        fit = i = next;
    }
    return fit;
}

int ui_text_fit(const ui_draw *d, ui_font f, int x, int y, int max_w, ui_color c, const char *s)
{
    size_t n = strlen(s);
    if (d->text_width(d->ctx, f, s, n) <= max_w) {
        return d->text(d->ctx, f, x, y, c, s, n);
    }
    int dots = d->text_width(d->ctx, f, UI_ELLIPSIS, sizeof UI_ELLIPSIS - 1);
    size_t keep = ui_fit(d, f, s, n, max_w - dots);
    while (keep > 0 && s[keep - 1] == ' ') {
        keep--;
    }
    int w = keep > 0 ? d->text(d->ctx, f, x, y, c, s, keep) : 0;
    if (dots <= max_w) {
        w += d->text(d->ctx, f, x + w, y, c, UI_ELLIPSIS, sizeof UI_ELLIPSIS - 1);
    }
    return w;
}

size_t ui_wrap(const ui_draw *d, ui_font f, const char *s, size_t n, int max_w, size_t *next)
{
    const char *nl = memchr(s, '\n', n);
    size_t line = nl != NULL ? (size_t)(nl - s) : n;
    size_t fit = ui_fit(d, f, s, line, max_w);
    size_t end = fit;
    if (fit < line) {
        /* Break at the last space that fits, if there is one past the line's start. */
        size_t space = fit;
        while (space > 0 && s[space] != ' ') {
            space--;
        }
        if (space > 0) {
            end = space;
        } else if (fit == 0) {
            end = ui_utf8_next(s, n, 0); /* not even one code point fits: take it anyway */
        }
    }
    size_t after = end;
    while (after < n && s[after] == ' ') {
        after++;
    }
    if (after < n && s[after] == '\n' && after == line) {
        after++;
    }
    while (end > 0 && s[end - 1] == ' ') {
        end--;
    }
    *next = after;
    return end;
}

int ui_text_wrap(const ui_draw *d, ui_font f, ui_rect r, ui_color c, const char *s)
{
    int lh = d->line_height(d->ctx, f);
    int max_lines = lh > 0 ? r.h / lh : 0;
    size_t n = strlen(s);
    size_t at = 0;
    int lines = 0;
    while (at < n && lines < max_lines) {
        size_t next = 0;
        size_t len = ui_wrap(d, f, s + at, n - at, r.w, &next);
        int y = r.y + lines * lh;
        if (lines + 1 == max_lines && at + next < n) {
            /* The last line that fits, with more to come: end it with an ellipsis, making
               room for it if the line is full. */
            int dots = d->text_width(d->ctx, f, UI_ELLIPSIS, sizeof UI_ELLIPSIS - 1);
            size_t keep = ui_fit(d, f, s + at, len, r.w - dots);
            while (keep > 0 && s[at + keep - 1] == ' ') {
                keep--;
            }
            int w = keep > 0 ? d->text(d->ctx, f, r.x, y, c, s + at, keep) : 0;
            (void)d->text(d->ctx, f, r.x + w, y, c, UI_ELLIPSIS, sizeof UI_ELLIPSIS - 1);
        } else {
            (void)d->text(d->ctx, f, r.x, y, c, s + at, len);
        }
        lines++;
        at += next;
    }
    return lines;
}
