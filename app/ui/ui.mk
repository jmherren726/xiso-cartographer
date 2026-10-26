# The UI (ADR 0015) for an XBE: its sources, SDL2 + SDL_ttf, and the bundled font.
# Include after setting ROOT_DIR, before nxdk's Makefile; the assets rules need OUTPUT_DIR,
# so `include $(ROOT_DIR)/app/ui/ui-assets.mk` after nxdk's Makefile too.

UI_DIR := $(ROOT_DIR)/app/ui
SRCS += $(UI_DIR)/input.c $(UI_DIR)/keyboard.c $(UI_DIR)/text.c $(UI_DIR)/theme.c \
        $(UI_DIR)/widgets.c $(UI_DIR)/draw_sdl.c $(UI_DIR)/input_sdl.c
CFLAGS += -I$(UI_DIR)

# nxdk's SDL2 port; SDL_ttf and FreeType come with it.
NXDK_SDL = y
