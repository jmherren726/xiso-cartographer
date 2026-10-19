# miniz (third_party/miniz, pinned submodule; ADR 0002, ADR 0009): only its inflater, tinfl,
# for streaming Deflate zip members (docs/formats/zip.md §9). Built as-is like BearSSL: no
# project warning flags, and SYSTEM includes so its headers can't fail -Werror.
#
# No allocation (the zip module owns the decompressor), no stdio, no time, no zip, zlib or
# deflate APIs. Users see the same defines, so the headers declare only what is built.
# app/Makefile uses the same flags for the XBE.
set(XCAT_MINIZ_DEFINES
  MINIZ_NO_MALLOC MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ARCHIVE_APIS MINIZ_NO_ZLIB_APIS
  MINIZ_NO_DEFLATE_APIS)

set(_mz_dir ${PROJECT_SOURCE_DIR}/third_party/miniz)
if(NOT EXISTS ${_mz_dir}/miniz_tinfl.c)
  message(FATAL_ERROR "third_party/miniz is missing: run git submodule update --init")
endif()
add_library(miniz_tinfl STATIC ${_mz_dir}/miniz_tinfl.c)
target_include_directories(miniz_tinfl SYSTEM PUBLIC ${_mz_dir} ${PROJECT_SOURCE_DIR}/cmake/miniz)
target_compile_definitions(miniz_tinfl PUBLIC ${XCAT_MINIZ_DEFINES})
if(MSVC)
  target_compile_options(miniz_tinfl PRIVATE /W0)
else()
  target_compile_options(miniz_tinfl PRIVATE -w)
endif()
