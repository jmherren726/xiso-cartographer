/* The ui_draw backend on SDL2 and SDL_ttf (draw_sdl.h, ADR 0015). */
#include "draw_sdl.h"

#include <SDL.h>
#include <SDL_ttf.h>
#include <hal/debug.h>
#include <hal/video.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#define CACHE_SLOTS 128    /* distinct strings kept as textures: a screen's worth, twice */
#define CACHE_TEXT_MAX 256 /* longer strings are rendered every time, not cached */
/* The NV2A register holding the physical address the screen shows (nxdk's hal/video.c keeps
   it private). Not XVideoSetFB: it takes a physical address and makes XVideoGetFB return it,
   which the CPU can't write through. */
#define PCRTC_START 0x00600800

/* Point sizes per font (ui_font); the line height comes from the font itself. */
static const struct {
    const char *file;
    int size;
} font_spec[UI_FONT_COUNT_] = {
    [UI_FONT_BODY] = {"NotoSans-Regular.ttf", 20},  /* list lines, text */
    [UI_FONT_BOLD] = {"NotoSans-Bold.ttf", 20},     /* the selected line */
    [UI_FONT_TITLE] = {"Michroma-Regular.ttf", 22}, /* titles; wide, so keep them short */
    [UI_FONT_SMALL] = {"NotoSans-Regular.ttf", 16}, /* details, hints */
    [UI_FONT_LABEL] = {"ChakraPetch-Bold.ttf", 21}, /* section names, labels */
};

typedef struct slot {
    SDL_Texture *tex; /* NULL: empty */
    int w, h;
    ui_font font;
    ui_color color;
    unsigned long used; /* the frame it was last drawn in, for eviction */
    char text[CACHE_TEXT_MAX + 1];
} slot;

static SDL_Window *window;
static SDL_Renderer *renderer;
static TTF_Font *fonts[UI_FONT_COUNT_];
static slot cache[CACHE_SLOTS];
static unsigned long frame;
static unsigned long lookups;
static unsigned long misses;
static char scratch[1024];   /* the n bytes of a string, NUL-terminated for SDL_ttf */
static unsigned char *fb[2]; /* [0] XVideoSetMode's framebuffer, [1] ours (ui_sdl_present) */
static int shown;            /* the one on screen */

/* The files of ui_image, in the XBE's ui\ folder (app/ui/ui-assets.mk). */
static const char *const image_file[UI_IMAGE_COUNT_] = {
    [UI_IMAGE_BACKGROUND] = "background.bmp",
};

static struct {
    SDL_Texture *tex; /* NULL: not loaded */
    int w, h;
} images[UI_IMAGE_COUNT_];

/* s[0, n) as a C string in `scratch`, cut at its size. */
static const char *terminated(const char *s, size_t n)
{
    if (n >= sizeof scratch) {
        n = sizeof scratch - 1;
    }
    memcpy(scratch, s, n);
    scratch[n] = '\0';
    return scratch;
}

static void sdl_fill(void *ctx, ui_rect r, ui_color c)
{
    (void)ctx;
    SDL_Rect rect = {r.x, r.y, r.w, r.h};
    SDL_SetRenderDrawBlendMode(renderer, c.a == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(renderer, &rect);
}

/* The cached texture of `s` in font f and color c, rendering it on a miss (into the least
   recently drawn slot). NULL if it can't be rendered, or for an empty string. */
static slot *texture_for(ui_font f, ui_color c, const char *s)
{
    lookups++;
    slot *victim = &cache[0];
    for (int i = 0; i < CACHE_SLOTS; i++) {
        slot *e = &cache[i];
        if (e->tex != NULL && e->font == f && memcmp(&e->color, &c, sizeof c) == 0 &&
            strcmp(e->text, s) == 0) {
            e->used = frame;
            return e;
        }
        if (e->tex == NULL || (victim->tex != NULL && e->used < victim->used)) {
            victim = e;
        }
    }
    misses++;
    SDL_Color color = {c.r, c.g, c.b, c.a};
    SDL_Surface *surface = TTF_RenderUTF8_Blended(fonts[f], s, color);
    if (surface == NULL) {
        return NULL;
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surface);
    int w = surface->w;
    int h = surface->h;
    SDL_FreeSurface(surface);
    if (tex == NULL) {
        return NULL;
    }
    if (victim->tex != NULL) {
        SDL_DestroyTexture(victim->tex);
    }
    victim->tex = tex;
    victim->w = w;
    victim->h = h;
    victim->font = f;
    victim->color = c;
    victim->used = frame;
    snprintf(victim->text, sizeof victim->text, "%s", s);
    return victim;
}

static int sdl_text(void *ctx, ui_font f, int x, int y, ui_color c, const char *s, size_t n)
{
    (void)ctx;
    if (n == 0) {
        return 0;
    }
    const char *t = terminated(s, n);
    if (n > CACHE_TEXT_MAX) {
        /* Too long to cache: render, draw and drop it. */
        SDL_Color color = {c.r, c.g, c.b, c.a};
        SDL_Surface *surface = TTF_RenderUTF8_Blended(fonts[f], t, color);
        if (surface == NULL) {
            return 0;
        }
        int w = surface->w;
        SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surface);
        if (tex != NULL) {
            SDL_Rect dst = {x, y, surface->w, surface->h};
            SDL_RenderCopy(renderer, tex, NULL, &dst);
            SDL_DestroyTexture(tex);
        }
        SDL_FreeSurface(surface);
        return w;
    }
    slot *e = texture_for(f, c, t);
    if (e == NULL) {
        return 0;
    }
    SDL_Rect dst = {x, y, e->w, e->h};
    SDL_RenderCopy(renderer, e->tex, NULL, &dst);
    return e->w;
}

static int sdl_text_width(void *ctx, ui_font f, const char *s, size_t n)
{
    (void)ctx;
    int w = 0;
    if (n > 0 && TTF_SizeUTF8(fonts[f], terminated(s, n), &w, NULL) != 0) {
        w = 0;
    }
    return w;
}

static int sdl_line_height(void *ctx, ui_font f)
{
    (void)ctx;
    return TTF_FontLineSkip(fonts[f]);
}

/* SDL_RenderSetClipRect, with the viewport queued first. nxdk's SDL (2.0.10's code) forgets
   the queued viewport at every flush, and a batch that starts with a clip rect then fails
   an SDL_assert in the software renderer, whose handler exits: a reboot. */
static void set_clip(const SDL_Rect *rect)
{
    SDL_RenderSetViewport(renderer, NULL);
    SDL_RenderSetClipRect(renderer, rect);
}

static void sdl_clip(void *ctx, const ui_rect *r)
{
    (void)ctx;
    if (r == NULL) {
        set_clip(NULL);
        return;
    }
    SDL_Rect rect = {r->x, r->y, r->w, r->h};
    set_clip(&rect);
}

static void sdl_image(void *ctx, ui_image img, ui_rect src, int x, int y)
{
    (void)ctx;
    if (images[img].tex == NULL) {
        return;
    }
    SDL_Rect from = {src.x, src.y, src.w, src.h};
    SDL_Rect to = {x, y, src.w, src.h};
    SDL_RenderCopy(renderer, images[img].tex, &from, &to);
}

static void sdl_image_size(void *ctx, ui_image img, int *w, int *h)
{
    (void)ctx;
    *w = images[img].tex != NULL ? images[img].w : 0;
    *h = images[img].tex != NULL ? images[img].h : 0;
}

/* Loads the images that exist; a missing one is drawn without (theme.c falls back). */
static void load_images(const char *asset_dir)
{
    for (int i = 0; i < UI_IMAGE_COUNT_; i++) {
        char path[260];
        snprintf(path, sizeof path, "%s\\ui\\%s", asset_dir, image_file[i]);
        SDL_Surface *surface = SDL_LoadBMP(path);
        if (surface == NULL) {
            continue;
        }
        images[i].tex = SDL_CreateTextureFromSurface(renderer, surface);
        images[i].w = surface->w;
        images[i].h = surface->h;
        SDL_FreeSurface(surface);
        if (images[i].tex != NULL) {
            /* Opaque: a plain copy, much cheaper than blending in software. */
            SDL_SetTextureBlendMode(images[i].tex, SDL_BLENDMODE_NONE);
        }
    }
}

/* A failed SDL_assert: SDL's handler would exit, which reboots without a word. Show it on
   the debug console instead, and stop. */
static SDL_AssertState on_assert(const SDL_AssertData *a, void *userdata)
{
    (void)userdata;
    XVideoWaitForVBlank();
    VIDEOREG(PCRTC_START) = (unsigned int)XVideoGetFB() & 0x7FFFFFFF;
    debugPrint("SDL assertion failed: %s\n%s:%d (%s)\n", a->condition, a->filename, a->linenum,
               a->function);
    for (;;) {
        Sleep(1000);
    }
}

bool ui_sdl_open(ui_draw *d, const char *asset_dir, const char **why)
{
    static char message[160];
    SDL_SetAssertionHandler(on_assert, NULL);
    if (SDL_VideoInit(NULL) != 0 || TTF_Init() != 0) {
        snprintf(message, sizeof message, "SDL video or TTF init failed: %s", SDL_GetError());
        *why = message;
        return false;
    }
    window = SDL_CreateWindow("xiso-cartographer", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              UI_SCREEN_W, UI_SCREEN_H, SDL_WINDOW_SHOWN);
    renderer = window != NULL ? SDL_CreateRenderer(window, -1, 0) : NULL;
    if (renderer == NULL) {
        snprintf(message, sizeof message, "SDL window or renderer failed: %s", SDL_GetError());
        *why = message;
        return false;
    }
    /* A second framebuffer, as XVideoSetMode allocates the first. Without it, frames are
       presented into the shown one (ui_sdl_present). */
    VIDEO_MODE vm = XVideoGetMode();
    fb[0] = XVideoGetFB();
    fb[1] = MmAllocateContiguousMemoryEx((SIZE_T)vm.width * vm.height * ((vm.bpp + 7) / 8), 0,
                                         0x7FFFFFFF, 0x1000, PAGE_READWRITE | PAGE_WRITECOMBINE);
    shown = 0;
    /* Only the bundled font is ever opened: nxdk's FreeType is old (ADR 0015). */
    for (int f = 0; f < UI_FONT_COUNT_; f++) {
        char path[260];
        snprintf(path, sizeof path, "%s\\fonts\\%s", asset_dir, font_spec[f].file);
        fonts[f] = TTF_OpenFont(path, font_spec[f].size);
        if (fonts[f] == NULL) {
            snprintf(message, sizeof message, "Can't open the font %s: %s", path, TTF_GetError());
            *why = message;
            return false;
        }
    }
    load_images(asset_dir);
    d->ctx = NULL;
    d->fill = sdl_fill;
    d->text = sdl_text;
    d->text_width = sdl_text_width;
    d->line_height = sdl_line_height;
    d->clip = sdl_clip;
    d->image = sdl_image;
    d->image_size = sdl_image_size;
    return true;
}

void ui_sdl_begin(ui_color bg)
{
    frame++;
    set_clip(NULL);
    SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, 255);
    SDL_RenderClear(renderer);
}

void ui_sdl_present(void)
{
    /* nxdk's SDL would copy the frame into the framebuffer on screen (SDL_RenderPresent),
       and the TV could show half of two frames. So the frame goes into the buffer that isn't
       shown, and the screen switches to it at the vertical blank. The software renderer draws into
       the window surface, but batches its commands (nxdk's SDL is 2.0.10's code, though
       SDL_version.h says 2.0.9): run them first. */
    SDL_RenderFlush(renderer);
    SDL_Surface *surface = SDL_GetWindowSurface(window);
    unsigned char *back = fb[!shown];
    if (surface == NULL || back == NULL) {
        SDL_RenderPresent(renderer);
        return;
    }
    VIDEO_MODE vm = XVideoGetMode();
    SDL_ConvertPixels(surface->w, surface->h, surface->format->format, surface->pixels,
                      surface->pitch, surface->format->format, back,
                      vm.width * SDL_BYTESPERPIXEL(surface->format->format));
    XVideoFlushFB();
    XVideoWaitForVBlank();
    VIDEOREG(PCRTC_START) = (unsigned int)back & 0x7FFFFFFF;
    shown = !shown;
}

void ui_sdl_cache_stats(unsigned long *l, unsigned long *m)
{
    *l = lookups;
    *m = misses;
}

void ui_sdl_close(void)
{
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (cache[i].tex != NULL) {
            SDL_DestroyTexture(cache[i].tex);
            cache[i].tex = NULL;
        }
    }
    for (int i = 0; i < UI_IMAGE_COUNT_; i++) {
        if (images[i].tex != NULL) {
            SDL_DestroyTexture(images[i].tex);
            images[i].tex = NULL;
        }
    }
    for (int f = 0; f < UI_FONT_COUNT_; f++) {
        if (fonts[f] != NULL) {
            TTF_CloseFont(fonts[f]);
            fonts[f] = NULL;
        }
    }
    if (renderer != NULL) {
        SDL_DestroyRenderer(renderer);
    }
    if (window != NULL) {
        SDL_DestroyWindow(window);
    }
    renderer = NULL;
    window = NULL;
    if (fb[1] != NULL) {
        if (shown != 0) {
            XVideoWaitForVBlank();
            VIDEOREG(PCRTC_START) = (unsigned int)fb[0] & 0x7FFFFFFF;
            shown = 0;
        }
        MmFreeContiguousMemory(fb[1]);
        fb[1] = NULL;
    }
    TTF_Quit();
}
