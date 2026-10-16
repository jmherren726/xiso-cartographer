# TLS for an XBE (ADR 0005, ADR 0012): BearSSL, core/src/tls.c and the built-in CA bundle.
# Included by an app Makefile after it sets ROOT_DIR and CORE_DIR, and before it includes
# nxdk's Makefile; it adds to SRCS and CFLAGS.

# The rules below come before nxdk's "all"; keep that the default goal.
.DEFAULT_GOAL := all

BEARSSL_DIR := $(ROOT_DIR)/third_party/bearssl
TLS_GEN_DIR := $(ROOT_DIR)/build/nxdk
CACERT_PEM := $(ROOT_DIR)/third_party/cacert/cacert.pem
CACERT_C := $(TLS_GEN_DIR)/cacert_data.c

BEARSSL_SRCS := $(shell find $(BEARSSL_DIR)/src -name '*.c')
TLS_SRCS := $(CORE_DIR)/src/tls.c $(CACERT_C)
NX_TLS_SRC := $(ROOT_DIR)/platform/nxdk/src/nx_tls.c
SRCS += $(TLS_SRCS) $(NX_TLS_SRC) $(BEARSSL_SRCS)
CFLAGS += -I$(BEARSSL_DIR)/inc

# Same as cmake/BearSSL.cmake: BearSSL finds neither randomness nor the time by itself, plus
# none of the x86 extensions a Pentium III lacks (SSE2, AES-NI, RDRAND).
BEARSSL_DEFINES := -DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0 -DBR_USE_WIN32_RAND=0 -DBR_RDRAND=0 \
	-DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_TIME=0 -DBR_LE_UNALIGNED=0 -DBR_BE_UNALIGNED=0 \
	-DBR_SSE2=0 -DBR_AES_X86NI=0 -DBR_POWER8=0 -DBR_INT128=0 -DBR_UMUL128=0

# nxdk compiles without optimisation by default; crypto at -O0 is several times slower, which
# would hide the real TLS speed (the M5 go/no-go measurement). BearSSL's own warnings aren't
# ours to fix (-w). nxdk's clang targets i386-pc-win32, so BearSSL sees both "clang" and
# "MSVC" and its intrinsics files include cpuid.h before intrin.h; including intrin.h first
# avoids the clash (as for clang on Windows in cmake/BearSSL.cmake).
BEARSSL_OBJS := $(addsuffix .obj, $(basename $(BEARSSL_SRCS)))
$(BEARSSL_OBJS): CFLAGS += -O2 -w $(BEARSSL_DEFINES) -I$(BEARSSL_DIR)/src -include intrin.h
TLS_OBJS := $(addsuffix .obj, $(basename $(TLS_SRCS)))
$(TLS_OBJS): CFLAGS += -O2 -I$(CORE_DIR)/src

# The console's clock is refused when it's earlier than this (ADR 0012 §5): the build time.
$(addsuffix .obj, $(basename $(NX_TLS_SRC))): CFLAGS += -DXC_BUILD_UNIX_TIME=$(shell date +%s)

$(CACERT_C): $(CACERT_PEM) $(CACERT_PEM).sha256 $(ROOT_DIR)/tools/cacert/embed_cacert.py
	@mkdir -p $(TLS_GEN_DIR)
	@echo "[ CACERT   ] $@"
	$(VE)python3 $(ROOT_DIR)/tools/cacert/embed_cacert.py --in $(CACERT_PEM) --out $@
