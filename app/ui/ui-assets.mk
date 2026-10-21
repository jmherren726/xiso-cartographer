# The UI's files next to the XBE (ADR 0015, docs/ui-style.md). Include after nxdk's Makefile,
# which sets OUTPUT_DIR.
#
# fonts\: the bundled fonts (third_party/fonts, ADR 0015 decision 7), after checking them against
#   third_party/fonts/SHA256SUMS. The check's stamp stays outside the XBE's folder, so it
#   isn't shipped; the copies themselves are the targets, so a cleaned folder gets them again.
# ui\: the images (app/ui/assets), converted from PNG to the BMP that nxdk's SDL loads
#   without SDL2_image (tools/ui/png2bmp.py, so the build needs python3, which the image has).

FONT_SRC := $(ROOT_DIR)/third_party/fonts
FONT_FILES := NotoSans-Regular.ttf NotoSans-Bold.ttf OFL.txt \
              Michroma-Regular.ttf OFL-Michroma.txt ChakraPetch-Bold.ttf OFL-ChakraPetch.txt
FONT_OUT := $(addprefix $(OUTPUT_DIR)/fonts/,$(FONT_FILES))

$(CURDIR)/fonts.stamp: $(addprefix $(FONT_SRC)/,$(FONT_FILES)) $(FONT_SRC)/SHA256SUMS
	@echo "[ FONTS    ] checking $(FONT_SRC)"
	$(VE)cd $(FONT_SRC) && sha256sum -c SHA256SUMS > /dev/null # GNU and BusyBox alike
	@touch $@

$(OUTPUT_DIR)/fonts/%: $(FONT_SRC)/% $(CURDIR)/fonts.stamp
	@mkdir -p $(dir $@)
	$(VE)cp $< $@

UI_ASSET_SRC := $(ROOT_DIR)/app/ui/assets
UI_IMAGES := background
UI_IMAGE_OUT := $(addprefix $(OUTPUT_DIR)/ui/,$(addsuffix .bmp,$(UI_IMAGES)))

$(OUTPUT_DIR)/ui/%.bmp: $(UI_ASSET_SRC)/%.png $(ROOT_DIR)/tools/ui/png2bmp.py
	@echo "[ IMAGE    ] $(notdir $@)"
	@mkdir -p $(dir $@)
	$(VE)python3 -I $(ROOT_DIR)/tools/ui/png2bmp.py $< $@

all: $(FONT_OUT) $(UI_IMAGE_OUT)
$(GEN_XISO): $(FONT_OUT) $(UI_IMAGE_OUT)
