# Unity (third_party/unity, pinned submodule) is built as-is: no project warning
# flags, and SYSTEM includes so its headers can't fail -Werror in our tests.
add_library(unity STATIC ${PROJECT_SOURCE_DIR}/third_party/unity/src/unity.c)
target_include_directories(unity SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/unity/src)
# Image offsets are 64-bit, so the 64-bit assertions must always be available.
target_compile_definitions(unity PUBLIC UNITY_SUPPORT_64)

# xcat_add_test(test_foo) builds test_foo.c into an executable and registers it with CTest.
function(xcat_add_test name)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE xcat::xcat unity)
  xcat_target_warnings(${name})
  add_test(NAME ${name} COMMAND ${name})
  if(XCAT_ASAN_RUNTIME_DIR)
    # LLVM's ASan DLL must win over MSVC's identically named one on PATH.
    set_tests_properties(${name} PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${XCAT_ASAN_RUNTIME_DIR}")
  endif()
endfunction()

xcat_add_test(test_version)
