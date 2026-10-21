# UI test XBE (see main.c; M8, ADR 0015). Built by the top-level Makefile: `make docker-xbe`.
# Output: app/ui-test/bin/default.xbe, its fonts\ folder, and app/ui-test/xcat-ui-test.iso.

XBE_TITLE = xcat-ui-test
GEN_XISO = $(XBE_TITLE).iso

ROOT_DIR := $(CURDIR)/../..
CORE_DIR := $(ROOT_DIR)/core

SRCS = $(CURDIR)/main.c \
       $(CORE_DIR)/src/version.c

CFLAGS += -I$(CORE_DIR)/include -std=c11 -Wall -Wextra -Werror

include $(ROOT_DIR)/app/ui/ui.mk

NXDK_DIR ?= $(ROOT_DIR)/third_party/nxdk

include $(NXDK_DIR)/Makefile

include $(ROOT_DIR)/app/ui/ui-assets.mk
