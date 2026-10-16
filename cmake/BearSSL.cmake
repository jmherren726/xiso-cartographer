# BearSSL (third_party/bearssl, pinned submodule; ADR 0005, ADR 0012), built as-is: no
# project warning flags, and SYSTEM includes so its headers can't fail -Werror.
#
# It's built with no way to find randomness or the time by itself: the tls module always
# supplies both from the platform (ADR 0012 §5, §6), so a missing source fails loudly
# instead of silently using something else. Unaligned access is off so UBSan stays quiet.
# app/Makefile uses the same flags for the XBE (plus the x86 extensions the Xbox lacks).
set(XCAT_BEARSSL_DEFINES
  BR_USE_URANDOM=0 BR_USE_GETENTROPY=0 BR_USE_WIN32_RAND=0 BR_RDRAND=0
  BR_USE_UNIX_TIME=0 BR_USE_WIN32_TIME=0
  BR_LE_UNALIGNED=0 BR_BE_UNALIGNED=0)

set(_br_dir ${PROJECT_SOURCE_DIR}/third_party/bearssl)
if(NOT EXISTS ${_br_dir}/inc/bearssl.h)
  message(FATAL_ERROR "third_party/bearssl is missing: run git submodule update --init")
endif()
file(GLOB_RECURSE _br_sources CONFIGURE_DEPENDS ${_br_dir}/src/*.c)
add_library(bearssl STATIC ${_br_sources})
target_include_directories(bearssl SYSTEM PUBLIC ${_br_dir}/inc)
target_include_directories(bearssl PRIVATE ${_br_dir}/src)
target_compile_definitions(bearssl PRIVATE ${XCAT_BEARSSL_DEFINES})
if(MSVC)
  # BearSSL's own code isn't ours to fix; keep the build log readable.
  target_compile_options(bearssl PRIVATE /W0)
else()
  target_compile_options(bearssl PRIVATE -w)
endif()
if(XCAT_SANITIZE AND NOT WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "Clang")
  # BearSSL's objects are vtables of functions that take their own context type, called
  # through pointers that take the generic one. Clang's UBSan "function" check flags every
  # such call; the pattern is BearSSL's design, so only that check is off, only here.
  target_compile_options(bearssl PRIVATE -fno-sanitize=function)
endif()
if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "Clang")
  # clang targeting MSVC is both BR_CLANG and BR_MSC to BearSSL, so its intrinsics files
  # include cpuid.h (a __cpuid macro) before intrin.h (a __cpuid function), which breaks.
  # Including intrin.h first avoids that.
  if(MSVC)
    target_compile_options(bearssl PRIVATE /FIintrin.h)
  else()
    target_compile_options(bearssl PRIVATE "SHELL:-include intrin.h")
  endif()
endif()
