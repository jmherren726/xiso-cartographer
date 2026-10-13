# XBE build (nxdk). Run through the top-level Makefile: `make xbe` inside the
# nxdk environment, or `make docker-xbe` from any machine with Docker.
#
# Output: app/bin/default.xbe and app/xiso-cartographer.iso (bootable in xemu).

XBE_TITLE = xiso-cartographer
GEN_XISO = $(XBE_TITLE).iso

CORE_DIR := $(CURDIR)/../core

# libxcat is compiled straight into the XBE: core/ is portable C with no
# platform dependencies, so it needs no separate library build here.
SRCS = $(CURDIR)/main.c \
       $(CORE_DIR)/src/version.c

CFLAGS += -I$(CORE_DIR)/include -std=c11 -Wall -Wextra -Werror

NXDK_DIR ?= $(CURDIR)/../third_party/nxdk

include $(NXDK_DIR)/Makefile
