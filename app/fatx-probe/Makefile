# FATX probe XBE (see main.c). Built by the top-level Makefile: `make docker-xbe`.
# Output: app/fatx-probe/bin/default.xbe and app/fatx-probe/xcat-fatx-probe.iso.

XBE_TITLE = xcat-fatx-probe
GEN_XISO = $(XBE_TITLE).iso
SRCS = $(CURDIR)/main.c
CFLAGS += -std=c11 -Wall -Wextra -Werror

NXDK_DIR ?= $(CURDIR)/../../third_party/nxdk

include $(NXDK_DIR)/Makefile
