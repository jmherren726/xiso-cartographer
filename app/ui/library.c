/* The library screen (library.h). */
#include "library.h"

#include <stdio.h>

#include "theme.h"

#define PAD 10

static const char *const section_names[UI_LIB_SECTION_COUNT_] = {"GAMES", "DOWNLOADS", "INSTALLED",
                                                                 "SETTINGS"};

/* The title-safe area (32, 24, 576 x 432), as docs/ui-style.md#layout. */
static const ui_rect tab = {32, 24, 240, 46};
static const ui_rect nav = {32, 80, 170, 196};
static const ui_rect summary = {32, 284, 170, 116};
static const ui_rect list = {210, 80, 212, 320};
static const ui_rect detail = {430, 80, 178, 320};
static const ui_rect footer = {32, 408, 576, 48};

/* The list's lines, inside its panel: 8 pixels in at the sides, PAD at the top and bottom. */
static const ui_rect list_in = {218, 90, 196, 300};

void ui_library_open(ui_library *lib, const ui_lib_item *items, int count)
{
    *lib = (ui_library){0};
    lib->items = items;
    lib->count = count;
    lib->list.count = count;
}

int ui_library_rows(const ui_draw *d)
{
    return ui_list_rows(d, list_in);
}

static void ask(ui_library *lib, ui_lib_action what)
{
    if (what == UI_LIB_INSTALL) {
        (void)snprintf(lib->message, sizeof lib->message,
                       "Install %s to E:\\Games? It can be stopped with B and resumed later.",
                       lib->items[lib->list.cur].name);
        lib->dialog = (ui_dialog){"INSTALL", lib->message, {"INSTALL", "CANCEL", NULL}, 0, 1};
    } else {
        lib->dialog =
            (ui_dialog){"LEAVE", "Return to the dashboard?", {"LEAVE", "STAY", NULL}, 0, 1};
    }
    lib->asking = what;
    lib->backdrop = false;
}

ui_lib_action ui_library_input(ui_library *lib, ui_button b, int rows)
{
    if (lib->asking != UI_LIB_NONE) {
        int c = ui_dialog_input(&lib->dialog, b);
        if (c == UI_DIALOG_OPEN) {
            return UI_LIB_NONE;
        }
        ui_lib_action what = lib->asking;
        lib->asking = UI_LIB_NONE;
        return c == 0 ? what : UI_LIB_NONE;
    }
    if (b == UI_BTN_BACK) {
        ask(lib, UI_LIB_EXIT);
    } else if (lib->in_sections) {
        if (b == UI_BTN_UP && lib->section > 0) {
            lib->section--;
        } else if (b == UI_BTN_DOWN && lib->section + 1 < UI_LIB_SECTION_COUNT_) {
            lib->section++;
        } else if (b == UI_BTN_RIGHT || b == UI_BTN_A) {
            lib->in_sections = false;
        }
    } else if (b == UI_BTN_LEFT) {
        lib->in_sections = true;
    } else if (lib->section == UI_LIB_GAMES) {
        if (b == UI_BTN_A && lib->count > 0) {
            ask(lib, UI_LIB_INSTALL);
        } else {
            (void)ui_list_input(&lib->list, b, rows);
        }
    }
    return UI_LIB_NONE;
}

static const char *item_name(void *ctx, int i)
{
    const ui_library *lib = ctx;
    return lib->items[i].name;
}

static void draw_sections(const ui_draw *d, const ui_library *lib)
{
    ui_panel(d, nav, UI_CHAMFER, ui_col_panel, ui_col_border);
    int lh = d->line_height(d->ctx, UI_FONT_LABEL);
    for (int i = 0; i < UI_LIB_SECTION_COUNT_; i++) {
        ui_rect item = {nav.x + 8, nav.y + PAD + i * (lh + 16), nav.w - 16, lh + 10};
        bool cur = i == (int)lib->section;
        if (cur) {
            ui_panel(d, item, 8, ui_col_highlight,
                     lib->in_sections ? ui_col_highlight_border : ui_col_border);
        }
        (void)ui_text_fit(d, UI_FONT_LABEL, item.x + 10, item.y + 5, item.w - 14,
                          cur ? ui_col_text : ui_col_dim, section_names[i]);
    }
}

static void draw_summary(const ui_draw *d, const ui_library *lib)
{
    ui_panel(d, summary, UI_CHAMFER, ui_col_panel, ui_col_border);
    int x = summary.x + 14;
    int w = summary.w - 24;
    int y = summary.y + PAD;
    (void)ui_text_fit(d, UI_FONT_LABEL, x, y, w, ui_col_accent, "THIS VISIT");
    y += d->line_height(d->ctx, UI_FONT_LABEL) + 4;
    int installed = 0;
    int unfinished = 0;
    for (int i = 0; i < lib->count; i++) {
        installed += lib->items[i].state == UI_LIB_INSTALLED;
        unfinished += lib->items[i].state == UI_LIB_FAILED || lib->items[i].state == UI_LIB_STOPPED;
    }
    char line[48];
    int sh = d->line_height(d->ctx, UI_FONT_SMALL);
    (void)snprintf(line, sizeof line, "%d game%s listed", lib->count, lib->count == 1 ? "" : "s");
    (void)ui_text_fit(d, UI_FONT_SMALL, x, y, w, ui_col_text, line);
    y += sh;
    (void)snprintf(line, sizeof line, "%d installed", installed);
    (void)ui_text_fit(d, UI_FONT_SMALL, x, y, w, ui_col_text, line);
    y += sh;
    if (unfinished > 0) {
        (void)snprintf(line, sizeof line, "%d unfinished", unfinished);
        (void)ui_text_fit(d, UI_FONT_SMALL, x, y, w, ui_col_dim, line);
    }
}

static void draw_detail(const ui_draw *d, const ui_lib_item *it)
{
    ui_panel(d, detail, UI_CHAMFER, ui_col_panel, ui_col_border);
    if (it == NULL) {
        return;
    }
    int x = detail.x + PAD + 4;
    int w = detail.w - 2 * PAD - 4;
    int y = detail.y + PAD;
    int bottom = detail.y + detail.h - PAD;
    /* The name on up to two lines. */
    int bh = d->line_height(d->ctx, UI_FONT_BOLD);
    y += ui_text_wrap(d, UI_FONT_BOLD, (ui_rect){x, y, w, 2 * bh}, ui_col_accent, it->name) * bh;
    int sh = d->line_height(d->ctx, UI_FONT_SMALL);
    if (it->facts != NULL && it->facts[0] != '\0') {
        (void)ui_text_fit(d, UI_FONT_SMALL, x, y, w, ui_col_dim, it->facts);
        y += sh;
    }
    y += 6;
    if (it->state != UI_LIB_NOT_TRIED) {
        (void)ui_text_fit(d, UI_FONT_LABEL, x, y, w,
                          it->state == UI_LIB_INSTALLED ? ui_col_accent : ui_col_text,
                          it->state == UI_LIB_INSTALLED ? "INSTALLED"
                          : it->state == UI_LIB_STOPPED ? "STOPPED"
                                                        : "FAILED");
        y += d->line_height(d->ctx, UI_FONT_LABEL);
        if (it->note != NULL && it->note[0] != '\0') {
            y += ui_text_wrap(d, UI_FONT_SMALL, (ui_rect){x, y, w, 3 * sh}, ui_col_text, it->note) *
                 sh;
        }
        y += 6;
    }
    if (it->about != NULL && y < bottom) {
        (void)ui_text_wrap(d, UI_FONT_SMALL, (ui_rect){x, y, w, bottom - y}, ui_col_dim, it->about);
    }
}

static void draw_list(const ui_draw *d, ui_library *lib)
{
    ui_panel(d, list, UI_CHAMFER, ui_col_panel, ui_col_border);
    if (lib->section != UI_LIB_GAMES) {
        (void)ui_text_wrap(d, UI_FONT_BODY, list_in, ui_col_dim,
                           "Not here yet: this section comes in a later version.");
        return;
    }
    if (lib->count == 0) {
        (void)ui_text_wrap(d, UI_FONT_BODY, list_in, ui_col_dim,
                           "No games. Put a map in E:\\xcat\\map.txt, or images in "
                           "E:\\xcat\\isos.");
        return;
    }
    ui_list_draw(d, &lib->list, list_in, item_name, lib);
}

static void draw_screen(const ui_draw *d, ui_library *lib, int pan, const char *status)
{
    const ui_rect panels[] = {tab, nav, summary, list, detail, footer};
    ui_background_under(d, pan, panels, (int)(sizeof panels / sizeof panels[0]));

    ui_panel(d, tab, UI_CHAMFER, ui_col_panel, ui_col_border);
    int th = d->line_height(d->ctx, UI_FONT_TITLE);
    (void)ui_text_fit(d, UI_FONT_TITLE, tab.x + 22, tab.y + (tab.h - th) / 2, tab.w - 30,
                      ui_col_accent, "LIBRARY");

    draw_sections(d, lib);
    draw_summary(d, lib);
    draw_list(d, lib);
    bool games = lib->section == UI_LIB_GAMES && lib->count > 0;
    draw_detail(d, games ? &lib->items[lib->list.cur] : NULL);

    ui_panel(d, footer, 0, ui_col_panel, ui_col_border);
    int sh = d->line_height(d->ctx, UI_FONT_SMALL);
    int by = footer.y + (footer.h - 2 * sh) / 2;
    if (status != NULL) {
        (void)ui_text_fit(d, UI_FONT_SMALL, footer.x + 12, by, footer.w - 24, ui_col_text, status);
    }
    (void)ui_text_fit(d, UI_FONT_SMALL, footer.x + 12, by + sh, footer.w - 24, ui_col_dim,
                      lib->in_sections ? "Up Down section  Right or A games  Back exit"
                                       : "A install  LT RT page  Left sections  Back exit");
}

void ui_library_draw(const ui_draw *d, ui_library *lib, int pan, const char *status)
{
    if (lib->asking == UI_LIB_NONE) {
        lib->backdrop = false;
        draw_screen(d, lib, pan, status);
        return;
    }
    if (lib->backdrop) {
        ui_backdrop(d);
    } else {
        draw_screen(d, lib, pan, status);
        ui_backdrop_save(d);
        lib->backdrop = true;
    }
    ui_dialog_draw(d, &lib->dialog);
}
