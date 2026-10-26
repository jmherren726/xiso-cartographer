/* The library screen (library.h). */
#include "library.h"

#include <stdio.h>
#include <string.h>

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

/* The search's panel, right of the title tab, while GAMES is searched. */
static const ui_rect search_box = {280, 24, 328, 46};

void ui_library_open(ui_library *lib, const ui_lib_item *items, int count, int *shown)
{
    *lib = (ui_library){0};
    lib->items = items;
    lib->count = count;
    lib->shown = shown;
    for (int i = 0; i < count; i++) {
        shown[i] = i;
    }
    lib->list.count = count;
}

int ui_library_game(const ui_library *lib)
{
    return lib->list.count > 0 ? lib->shown[lib->list.cur] : -1;
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

/* True if s holds w[0, n), ASCII case-insensitive. */
static bool has_word(const char *s, const char *w, size_t n)
{
    if (s == NULL) {
        return false;
    }
    for (; *s != '\0'; s++) {
        size_t i = 0;
        while (i < n && s[i] != '\0' && lower(s[i]) == lower(w[i])) {
            i++;
        }
        if (i == n) {
            return true;
        }
    }
    return false;
}

bool ui_library_matches(const ui_lib_item *it, const char *query)
{
    const char *p = query;
    for (;;) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            return true;
        }
        size_t n = strcspn(p, " ");
        if (!has_word(it->name, p, n) && !has_word(it->facts, p, n) && !has_word(it->about, p, n)) {
            return false;
        }
        p += n;
    }
}

/* Lists the games matching lib->query, keeping the highlighted one if it's among them. */
static void search(ui_library *lib, int rows)
{
    int was = ui_library_game(lib);
    int n = 0;
    int cur = 0;
    for (int i = 0; i < lib->count; i++) {
        if (ui_library_matches(&lib->items[i], lib->query)) {
            if (i == was) {
                cur = n;
            }
            lib->shown[n++] = i;
        }
    }
    lib->list = (ui_list){n, cur, cur >= rows ? cur - rows + 1 : 0};
}

void ui_library_set_installed(ui_library *lib, const ui_lib_item *items, int count)
{
    lib->installed = items;
    lib->installed_count = count;
    ui_list *l = &lib->installed_list;
    l->count = count;
    if (l->cur >= count) {
        l->cur = count > 0 ? count - 1 : 0;
    }
    if (l->top > l->cur) {
        l->top = l->cur;
    }
}

/* The highlighted line of the section shown, or NULL. */
static const ui_lib_item *current(const ui_library *lib)
{
    if (lib->section == UI_LIB_GAMES && lib->list.count > 0) {
        return &lib->items[ui_library_game(lib)];
    }
    if (lib->section == UI_LIB_INSTALLED_GAMES && lib->installed_count > 0) {
        return &lib->installed[lib->installed_list.cur];
    }
    return NULL;
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
                       lib->items[ui_library_game(lib)].name);
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
    if (lib->searching) {
        ui_kbd_result r = ui_kbd_input(&lib->kbd, b);
        if (r != UI_KBD_EDITING) {
            lib->searching = false;
        }
        if (r == UI_KBD_DONE) {
            (void)snprintf(lib->query, sizeof lib->query, "%s", lib->typing);
            lib->section = UI_LIB_GAMES;
            lib->in_sections = false;
            search(lib, rows);
        }
        return UI_LIB_NONE;
    }
    if (b == UI_BTN_Y) {
        (void)snprintf(lib->typing, sizeof lib->typing, "%s", lib->query);
        ui_kbd_open(&lib->kbd, "SEARCH", lib->typing, sizeof lib->typing, false);
        lib->searching = true;
        lib->backdrop = false;
    } else if (b == UI_BTN_BACK) {
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
        if (b == UI_BTN_A && lib->list.count > 0) {
            ask(lib, UI_LIB_INSTALL);
        } else if (b == UI_BTN_B && lib->query[0] != '\0') {
            lib->query[0] = '\0';
            search(lib, rows);
        } else {
            (void)ui_list_input(&lib->list, b, rows);
        }
    } else if (lib->section == UI_LIB_INSTALLED_GAMES && b != UI_BTN_A) {
        (void)ui_list_input(&lib->installed_list, b, rows);
    }
    return UI_LIB_NONE;
}

static const char *item_name(void *ctx, int i)
{
    const ui_library *lib = ctx;
    return lib->items[lib->shown[i]].name;
}

static const char *installed_name(void *ctx, int i)
{
    const ui_library *lib = ctx;
    return lib->installed[i].name;
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
    (void)ui_text_fit(d, UI_FONT_LABEL, x, y, w, ui_col_accent, "SUMMARY");
    y += d->line_height(d->ctx, UI_FONT_LABEL) + 4;
    int installed = 0;
    int unfinished = 0;
    for (int i = 0; i < lib->count; i++) {
        installed += lib->items[i].state == UI_LIB_INSTALLED;
        unfinished += lib->items[i].state == UI_LIB_FAILED ||
                      lib->items[i].state == UI_LIB_STOPPED ||
                      lib->items[i].state == UI_LIB_UNFINISHED;
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
                          it->state == UI_LIB_INSTALLED    ? "INSTALLED"
                          : it->state == UI_LIB_STOPPED    ? "STOPPED"
                          : it->state == UI_LIB_UNFINISHED ? "UNFINISHED"
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
    if (lib->section == UI_LIB_INSTALLED_GAMES) {
        if (lib->installed_count == 0) {
            (void)ui_text_wrap(d, UI_FONT_BODY, list_in, ui_col_dim,
                               "Nothing installed yet. Games go to E:\\Games.");
        } else {
            ui_list_draw(d, &lib->installed_list, list_in, installed_name, lib);
        }
        return;
    }
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
    if (lib->list.count == 0) {
        char line[UI_LIB_QUERY_MAX + 64];
        (void)snprintf(line, sizeof line,
                       "No game matches \"%s\". Y to search again, B to list them all.",
                       lib->query);
        (void)ui_text_wrap(d, UI_FONT_BODY, list_in, ui_col_dim, line);
        return;
    }
    ui_list_draw(d, &lib->list, list_in, item_name, lib);
}

/* While GAMES is searched: what for, and how many games match. */
static void draw_search(const ui_draw *d, const ui_library *lib)
{
    ui_panel(d, search_box, UI_CHAMFER, ui_col_panel, ui_col_border);
    int lh = d->line_height(d->ctx, UI_FONT_LABEL);
    int sh = d->line_height(d->ctx, UI_FONT_SMALL);
    int x = search_box.x + 16;
    int right = search_box.x + search_box.w - 16;
    int y = search_box.y + (search_box.h - lh) / 2;
    x += ui_text_fit(d, UI_FONT_LABEL, x, y, right - x, ui_col_accent, "SEARCH") + 12;
    char n[32];
    (void)snprintf(n, sizeof n, "%d of %d", lib->list.count, lib->count);
    int nw = d->text_width(d->ctx, UI_FONT_SMALL, n, strlen(n));
    int sy = search_box.y + (search_box.h - sh) / 2;
    (void)ui_text_fit(d, UI_FONT_SMALL, right - nw, sy, nw, ui_col_dim, n);
    (void)ui_text_fit(d, UI_FONT_BODY, x,
                      search_box.y + (search_box.h - d->line_height(d->ctx, UI_FONT_BODY)) / 2,
                      right - nw - 12 - x, ui_col_text, lib->query);
}

static const char *hints(const ui_library *lib)
{
    if (lib->in_sections) {
        return "Up Down section  Right or A list  Y search  Back exit";
    }
    if (lib->section != UI_LIB_GAMES) {
        return "LT RT page  Left sections  Y search  Back exit";
    }
    return lib->query[0] != '\0' ? "A install  B list all  Y search  LT RT page  Left sections"
                                 : "A install  Y search  LT RT page  Left sections  Back exit";
}

static void draw_screen(const ui_draw *d, ui_library *lib, int pan, const char *status)
{
    bool searched = lib->query[0] != '\0';
    const ui_rect panels[] = {tab, nav, summary, list, detail, footer, search_box};
    ui_background_under(d, pan, panels, (int)(sizeof panels / sizeof panels[0]) - !searched);

    ui_panel(d, tab, UI_CHAMFER, ui_col_panel, ui_col_border);
    int th = d->line_height(d->ctx, UI_FONT_TITLE);
    (void)ui_text_fit(d, UI_FONT_TITLE, tab.x + 22, tab.y + (tab.h - th) / 2, tab.w - 30,
                      ui_col_accent, "LIBRARY");

    if (searched) {
        draw_search(d, lib);
    }
    draw_sections(d, lib);
    draw_summary(d, lib);
    draw_list(d, lib);
    draw_detail(d, current(lib));

    ui_panel(d, footer, 0, ui_col_panel, ui_col_border);
    int sh = d->line_height(d->ctx, UI_FONT_SMALL);
    int by = footer.y + (footer.h - 2 * sh) / 2;
    if (status != NULL) {
        (void)ui_text_fit(d, UI_FONT_SMALL, footer.x + 12, by, footer.w - 24, ui_col_text, status);
    }
    (void)ui_text_fit(d, UI_FONT_SMALL, footer.x + 12, by + sh, footer.w - 24, ui_col_dim,
                      hints(lib));
}

void ui_library_draw(const ui_draw *d, ui_library *lib, int pan, const char *status)
{
    if (lib->asking == UI_LIB_NONE && !lib->searching) {
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
    if (lib->searching) {
        ui_kbd_draw(d, &lib->kbd);
    } else {
        ui_dialog_draw(d, &lib->dialog);
    }
}
