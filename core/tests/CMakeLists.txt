# Unity (third_party/unity, pinned submodule) is built as-is: no project warning
# flags, and SYSTEM includes so its headers can't fail -Werror in our tests.
add_library(unity STATIC ${PROJECT_SOURCE_DIR}/third_party/unity/src/unity.c)
target_include_directories(unity SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/unity/src)
# Image offsets are 64-bit, so the 64-bit assertions must always be available.
target_compile_definitions(unity PUBLIC UNITY_SUPPORT_64
  # Use the C11 keyword directly. Otherwise Unity includes <stdnoreturn.h> under clang-cl,
  # whose `noreturn` macro breaks __declspec(noreturn) in the Windows SDK headers.
  UNITY_NORETURN=_Noreturn)

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

# Test code uses plain stdio (fopen); the CRT's "use fopen_s" deprecation doesn't apply.
function(xcat_test_target target)
  if(WIN32)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
endfunction()

# Puts LLVM's ASan DLL first on PATH for clang-cl builds; it must win over MSVC's
# identically named one.
function(xcat_test_env test)
  if(XCAT_ASAN_RUNTIME_DIR)
    set_tests_properties(${test} PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${XCAT_ASAN_RUNTIME_DIR}")
  endif()
endfunction()

# xcat_add_test(test_foo [ARGS ...]) builds test_foo.c into an executable and
# registers it with CTest, passing ARGS on the command line.
function(xcat_add_test name)
  cmake_parse_arguments(PARSE_ARGV 1 T "" "" "ARGS")
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE xcat::xcat xcat::host unity)
  xcat_test_target(${name})
  # Tests may use libxcat's internal headers (e.g. bytes.h).
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/core/src)
  xcat_target_warnings(${name})
  add_test(NAME ${name} COMMAND ${name} ${T_ARGS})
  xcat_test_env(${name})
endfunction()

# xcat_add_fuzzer(fuzz_foo SEEDS ...) builds fuzz_foo.c twice: as fuzz_foo_replay
# (any compiler; CTest runs it over SEEDS as test fuzz_foo_seeds) and, with
# XCAT_FUZZ, as the libFuzzer binary fuzz_foo. See CONTRIBUTING.md#fuzzing.
function(xcat_add_fuzzer name)
  cmake_parse_arguments(PARSE_ARGV 1 F "" "" "SEEDS")
  add_executable(${name}_replay ${name}.c support/fuzz_replay.c)
  target_link_libraries(${name}_replay PRIVATE xcat::xcat)
  xcat_test_target(${name}_replay)
  xcat_target_warnings(${name}_replay)
  add_test(NAME ${name}_seeds COMMAND ${name}_replay ${F_SEEDS})
  xcat_test_env(${name}_seeds)
  if(XCAT_FUZZ)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE xcat::xcat)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    xcat_target_warnings(${name})
  endif()
endfunction()

xcat_add_test(test_version)
xcat_add_test(test_bytes)
xcat_add_test(test_xdvdfs ARGS ${XCAT_FIXTURES_DIR} ${XCAT_FIXTURES})
add_dependencies(test_xdvdfs xcat_fixtures)
xcat_add_test(test_extract)
xcat_add_test(test_fatx)
xcat_add_test(test_url)
xcat_add_test(test_http)
xcat_add_test(test_http_client)

# FATX installability (ADR 0008): every fixture can be installed except "deep", whose names
# of 43 and 255 characters can't be stored on FATX; `xcat-cli check` must refuse it (exit 3).
foreach(_name IN LISTS XCAT_FIXTURES)
  add_test(NAME check_${_name} COMMAND xcat-cli check ${XCAT_FIXTURES_DIR}/${_name}.iso)
  xcat_test_env(check_${_name})
endforeach()
set_tests_properties(check_deep PROPERTIES WILL_FAIL TRUE)

# `xcat-cli layout` runs on every fixture (all have their tables first; test_xdvdfs checks the numbers).
foreach(_name IN LISTS XCAT_FIXTURES)
  add_test(NAME layout_${_name} COMMAND xcat-cli layout ${XCAT_FIXTURES_DIR}/${_name}.iso)
  set_tests_properties(layout_${_name} PROPERTIES PASS_REGULAR_EXPRESSION "one sequential pass can install it")
  xcat_test_env(layout_${_name})
endforeach()

# End to end: xcat-cli extracts every fixture, and the result must match its manifest (and so
# extract-xiso's output: tools/fixtures/verify.sh checks the manifests against extract-xiso).
foreach(_name IN LISTS XCAT_FIXTURES)
  add_test(NAME extract_${_name}
    COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/fixtures/extract_and_check.py
            --cli $<TARGET_FILE:xcat-cli> --fixtures ${XCAT_FIXTURES_DIR}
            --out ${CMAKE_BINARY_DIR}/extracted ${_name})
  xcat_test_env(extract_${_name})
endforeach()

# Fuzz seeds: the directory tables of the xiso fixtures, in the harness's input layout.
set(XCAT_FUZZ_SEEDS_DIR ${CMAKE_BINARY_DIR}/fuzz-seeds)
set(_seed_fixtures small deep)
set(_seed_outputs)
set(_seed_inputs)
foreach(_name IN LISTS _seed_fixtures)
  list(APPEND _seed_outputs ${XCAT_FUZZ_SEEDS_DIR}/${_name}.seed)
  list(APPEND _seed_inputs ${XCAT_FIXTURES_DIR}/${_name}.iso)
endforeach()
add_custom_command(
  OUTPUT ${_seed_outputs}
  COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/fixtures/fuzz_seeds.py
          --fixtures ${XCAT_FIXTURES_DIR} --out ${XCAT_FUZZ_SEEDS_DIR} ${_seed_fixtures}
  DEPENDS ${PROJECT_SOURCE_DIR}/tools/fixtures/fuzz_seeds.py ${_seed_inputs}
  COMMENT "Generating fuzz seeds"
  VERBATIM)
add_custom_target(xcat_fuzz_seeds ALL DEPENDS ${_seed_outputs})

xcat_add_fuzzer(fuzz_xdvdfs SEEDS ${_seed_outputs})
add_dependencies(fuzz_xdvdfs_replay xcat_fuzz_seeds)

# HTTP parser and URL seeds are committed (exact bytes; see .gitattributes).
file(GLOB _http_seeds ${CMAKE_CURRENT_SOURCE_DIR}/fuzz-seeds/http/*.seed)
xcat_add_fuzzer(fuzz_http SEEDS ${_http_seeds})
