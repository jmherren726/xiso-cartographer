# Unity (third_party/unity, pinned submodule) is built as-is: no project warning
# flags, and SYSTEM includes so its headers can't fail -Werror in our tests.
add_library(unity STATIC ${PROJECT_SOURCE_DIR}/third_party/unity/src/unity.c)
target_include_directories(unity SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/unity/src)
# Image offsets are 64-bit, so the 64-bit assertions must always be available.
target_compile_definitions(unity PUBLIC UNITY_SUPPORT_64
  # Use the C11 keyword directly. Otherwise Unity includes <stdnoreturn.h> under clang-cl,
  # whose `noreturn` macro breaks __declspec(noreturn) in the Windows SDK headers.
  UNITY_NORETURN=_Noreturn)

# Helpers shared by tests (host-only code; never part of libxcat).
add_library(xcat_test_support STATIC support/file_source.c)
target_include_directories(xcat_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xcat_test_support PUBLIC xcat::xcat)
# Test code uses plain stdio (fopen); the CRT's "use fopen_s" deprecation doesn't apply.
if(WIN32)
  target_compile_definitions(xcat_test_support PUBLIC _CRT_SECURE_NO_WARNINGS)
endif()
xcat_target_warnings(xcat_test_support)

# Synthetic xiso fixtures (tools/fixtures/), generated at build time.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
option(XCAT_LARGE_FIXTURES "Also generate and test the >4 GiB sparse fixture" OFF)
set(XCAT_FIXTURES_DIR ${CMAKE_BINARY_DIR}/fixtures)
set(XCAT_FIXTURES small deep redump-xgd1 redump-xgd2 redump-xgd3)
set(_fixture_args)
if(XCAT_LARGE_FIXTURES)
  list(APPEND XCAT_FIXTURES large)
  set(_fixture_args --large)
endif()
set(_fixture_outputs)
foreach(_name IN LISTS XCAT_FIXTURES)
  list(APPEND _fixture_outputs ${XCAT_FIXTURES_DIR}/${_name}.iso ${XCAT_FIXTURES_DIR}/${_name}.tsv)
endforeach()
add_custom_command(
  OUTPUT ${_fixture_outputs}
  COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/fixtures/fixtures.py
          --out ${XCAT_FIXTURES_DIR} ${_fixture_args}
  DEPENDS ${PROJECT_SOURCE_DIR}/tools/fixtures/fixtures.py
          ${PROJECT_SOURCE_DIR}/tools/fixtures/xisogen.py
  COMMENT "Generating xiso test fixtures"
  VERBATIM)
add_custom_target(xcat_fixtures ALL DEPENDS ${_fixture_outputs})

# xcat_add_test(test_foo [ARGS ...]) builds test_foo.c into an executable and
# registers it with CTest, passing ARGS on the command line.
function(xcat_add_test name)
  cmake_parse_arguments(PARSE_ARGV 1 T "" "" "ARGS")
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE xcat::xcat xcat_test_support unity)
  # Tests may use libxcat's internal headers (e.g. bytes.h).
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/core/src)
  xcat_target_warnings(${name})
  add_test(NAME ${name} COMMAND ${name} ${T_ARGS})
  if(XCAT_ASAN_RUNTIME_DIR)
    # LLVM's ASan DLL must win over MSVC's identically named one on PATH.
    set_tests_properties(${name} PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${XCAT_ASAN_RUNTIME_DIR}")
  endif()
endfunction()

xcat_add_test(test_version)
xcat_add_test(test_bytes)
xcat_add_test(test_xdvdfs ARGS ${XCAT_FIXTURES_DIR} ${XCAT_FIXTURES})
add_dependencies(test_xdvdfs xcat_fixtures)
