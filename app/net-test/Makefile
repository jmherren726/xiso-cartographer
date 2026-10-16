# Network test XBE (see main.c; M3, HTTPS since M5). Built by the top-level Makefile: `make docker-xbe`.
# Output: app/net-test/bin/default.xbe and app/net-test/xcat-net-test.iso.

XBE_TITLE = xcat-net-test
GEN_XISO = $(XBE_TITLE).iso

ROOT_DIR := $(CURDIR)/../..
CORE_DIR := $(ROOT_DIR)/core
PLATFORM_DIR := $(ROOT_DIR)/platform/nxdk

SRCS = $(CURDIR)/main.c \
       $(CORE_DIR)/src/crc32.c \
       $(CORE_DIR)/src/error.c \
       $(CORE_DIR)/src/http.c \
       $(CORE_DIR)/src/http_client.c \
       $(CORE_DIR)/src/url.c \
       $(CORE_DIR)/src/version.c \
       $(PLATFORM_DIR)/src/nx_net.c \
       $(PLATFORM_DIR)/src/nx_platform.c

CFLAGS += -I$(CORE_DIR)/include -I$(PLATFORM_DIR)/include -std=c11 -Wall -Wextra -Werror

# HTTPS (M5): BearSSL, core/src/tls.c, the CA bundle and platform/nxdk/src/nx_tls.c.
include $(CURDIR)/../tls.mk

NXDK_DIR ?= $(ROOT_DIR)/third_party/nxdk

# TCP tuning shared by every XBE (lwIP options).
include $(ROOT_DIR)/app/lwip.mk

include $(NXDK_DIR)/Makefile
