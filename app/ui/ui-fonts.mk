# Copies the bundled font (third_party/fonts, ADR 0015 decision 7) into the XBE's folder as
# fonts\, after checking it against third_party/fonts/SHA256SUMS. Include after nxdk's
# Makefile, which sets OUTPUT_DIR. The check's stamp stays outside the XBE's folder, so it
# isn't shipped; the copies themselves are the targets, so a cleaned folder gets them again.

FONT_SRC := $(ROOT_DIR)/third_party/fonts
FONT_FILES := NotoSans-Regular.ttf NotoSans-Bold.ttf OFL.txt
FONT_OUT := $(addprefix $(OUTPUT_DIR)/fonts/,$(FONT_FILES))

$(CURDIR)/fonts.stamp: $(addprefix $(FONT_SRC)/,$(FONT_FILES)) $(FONT_SRC)/SHA256SUMS
	@echo "[ FONTS    ] checking $(FONT_SRC)"
	$(VE)cd $(FONT_SRC) && sha256sum -c SHA256SUMS > /dev/null # GNU and BusyBox alike
	@touch $@

$(OUTPUT_DIR)/fonts/%: $(FONT_SRC)/% $(CURDIR)/fonts.stamp
	@mkdir -p $(dir $@)
	$(VE)cp $< $@

all: $(FONT_OUT)
$(GEN_XISO): $(FONT_OUT)
