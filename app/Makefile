# XBE build (nxdk). Run through the top-level Makefile: `make xbe` inside the
# nxdk environment, or `make docker-xbe` from any machine with Docker.
#
# Output: app/bin/default.xbe, app/bin/samples/small.iso (bundled test image) and
# app/xiso-cartographer.iso (bootable in xemu; contains both).

XBE_TITLE = xiso-cartographer
GEN_XISO = $(XBE_TITLE).iso

ROOT_DIR := $(CURDIR)/..
CORE_DIR := $(ROOT_DIR)/core
PLATFORM_DIR := $(ROOT_DIR)/platform/nxdk

# libxcat is compiled straight into the XBE: core/ is portable C with no
# platform dependencies, so it needs no separate library build here.
SRCS = $(CURDIR)/main.c \
       $(CORE_DIR)/src/crc32.c \
       $(CORE_DIR)/src/credentials.c \
       $(CORE_DIR)/src/error.c \
       $(CORE_DIR)/src/extract.c \
       $(CORE_DIR)/src/fatx.c \
       $(CORE_DIR)/src/http.c \
       $(CORE_DIR)/src/http_client.c \
       $(CORE_DIR)/src/install.c \
       $(CORE_DIR)/src/journal.c \
       $(CORE_DIR)/src/map.c \
       $(CORE_DIR)/src/source.c \
       $(CORE_DIR)/src/text.c \
       $(CORE_DIR)/src/url.c \
       $(CORE_DIR)/src/version.c \
       $(CORE_DIR)/src/xdvdfs.c \
       $(PLATFORM_DIR)/src/nx_install.c \
       $(PLATFORM_DIR)/src/nx_net.c \
       $(PLATFORM_DIR)/src/nx_platform.c

CFLAGS += -I$(CORE_DIR)/include -I$(PLATFORM_DIR)/include -std=c11 -Wall -Wextra -Werror

# SDL2 (prebuilt in the nxdk image) only for reading the controllers: the dashboard shortcut.
NXDK_SDL = y

NXDK_DIR ?= $(ROOT_DIR)/third_party/nxdk

# TCP tuning shared by every XBE (lwIP options).
include $(ROOT_DIR)/app/lwip.mk

include $(NXDK_DIR)/Makefile

# Bundled synthetic test image (the tools/fixtures "small" fixture: dummy data, never a
# game), so the M2 install can be tried without copying anything to the console first.
SAMPLE_ISO = $(OUTPUT_DIR)/samples/small.iso
$(SAMPLE_ISO): $(ROOT_DIR)/tools/fixtures/fixtures.py $(ROOT_DIR)/tools/fixtures/xisogen.py
	@mkdir -p $(OUTPUT_DIR)/samples
	@echo "[ SAMPLE   ] $@"
	$(VE)python3 $(ROOT_DIR)/tools/fixtures/fixtures.py --out $(OUTPUT_DIR)/samples --only small $(QUIET)
	$(VE)rm -f $(OUTPUT_DIR)/samples/small.json $(OUTPUT_DIR)/samples/small.tsv

all: $(SAMPLE_ISO)
$(GEN_XISO): $(SAMPLE_ISO)
