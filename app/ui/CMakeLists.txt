# The UI's portable part (ADR 0015 decision 4): everything in app/ui/ except *_sdl.c, which
# needs nxdk's SDL. Built on the PC only for its tests; the XBE compiles the same files
# through app/ui/ui.mk.
add_library(xcat_ui STATIC text.c theme.c)
target_include_directories(xcat_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
xcat_target_warnings(xcat_ui)

if(XCAT_BUILD_TESTS)
  foreach(_name IN ITEMS test_ui_text test_ui_theme)
    add_executable(${_name} tests/${_name}.c)
    target_link_libraries(${_name} PRIVATE xcat_ui unity)
    xcat_test_target(${_name})
    xcat_target_warnings(${_name})
    add_test(NAME ${_name} COMMAND ${_name})
    xcat_test_env(${_name})
  endforeach()

  # The PNG-to-BMP step of the XBE build (tools/ui/png2bmp.py), on made-up PNGs and on
  # app/ui/assets/background.png.
  find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
  add_test(NAME ui_png2bmp
    COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/ui/test_png2bmp.py)
  xcat_test_env(ui_png2bmp)
endif()
