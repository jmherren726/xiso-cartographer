# Compiler warnings and sanitizers for project targets (see docs/HANDOFF.md §7.1).
# Third-party targets don't use xcat_target_warnings(), so their warnings never fail our build.

function(xcat_target_warnings target)
  if(MSVC)
    # Covers both cl and clang-cl.
    target_compile_options(${target} PRIVATE /W4 $<$<BOOL:${XCAT_WERROR}>:/WX>)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic
      -Wshadow -Wconversion -Wstrict-prototypes -Wmissing-prototypes
      $<$<BOOL:${XCAT_WERROR}>:-Werror>)
  endif()
endfunction()

# Applies to every target in the build, so instrumented code is never linked
# against uninstrumented code.
macro(xcat_enable_sanitizers)
  if(XCAT_SANITIZE)
    if(MSVC)
      # MSVC-ABI ASan doesn't support the debug CRT, and UBSan isn't available.
      set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
      add_compile_options(/fsanitize=address)
      if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        # clang-cl: CMake links with lld-link directly, which (unlike cl's
        # link.exe) isn't told about the ASan runtime, so name it explicitly.
        execute_process(COMMAND ${CMAKE_C_COMPILER} --print-resource-dir
          OUTPUT_VARIABLE _xc_res OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
        file(TO_CMAKE_PATH "${_xc_res}/lib/windows" XCAT_ASAN_RUNTIME_DIR)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
          set(_xc_arch x86_64)
        else()
          set(_xc_arch i386)
        endif()
        add_link_options(
          "${XCAT_ASAN_RUNTIME_DIR}/clang_rt.asan_dynamic-${_xc_arch}.lib"
          "/wholearchive:${XCAT_ASAN_RUNTIME_DIR}/clang_rt.asan_dynamic_runtime_thunk-${_xc_arch}.lib")
      endif()
    elseif(WIN32)
      # GNU-style clang targeting the MSVC ABI: same CRT restriction.
      set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
      add_compile_options(-fsanitize=address)
      add_link_options(-fsanitize=address)
    else()
      set(_xc_san -fsanitize=address,undefined -fno-sanitize-recover=all)
      add_compile_options(${_xc_san} -fno-omit-frame-pointer)
      add_link_options(${_xc_san})
    endif()
  endif()
endmacro()
