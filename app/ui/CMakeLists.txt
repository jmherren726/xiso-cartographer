# The UI's portable part (ADR 0015 decision 4): everything in app/ui/ except *_sdl.c, which
# needs nxdk's SDL. Built on the PC only for its tests; the XBE compiles the same files
# through app/ui/ui.mk.
add_library(xcat_ui STATIC input.c install_view.c keyboard.c library.c script.c startup_view.c
            text.c theme.c widgets.c)
target_include_directories(xcat_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xcat_ui PUBLIC xcat::xcat) # script.c plays xcat/ui_script.h's scripts
xcat_target_warnings(xcat_ui)

if(XCAT_BUILD_TESTS)
  foreach(_name IN ITEMS test_ui_text test_ui_theme test_ui_input test_ui_widgets
                        test_ui_keyboard test_ui_library test_ui_install test_ui_script_run
                        test_ui_startup)
    add_executable(${_name} tests/${_name}.c tests/fake_draw.c)
    target_link_libraries(${_name} PRIVATE xcat_ui unity)
    xcat_test_target(${_name})
    xcat_target_warnings(${_name})
    add_test(NAME ${_name} COMMAND ${_name})
    xcat_test_env(${_name})
  endforeach()
  # It also checks the exit test's walkthrough (docs/testing.md).
  target_compile_definitions(test_ui_script_run PRIVATE
    XCAT_WALKTHROUGH="${PROJECT_SOURCE_DIR}/tools/ui/walkthrough.txt")

  # The PNG-to-BMP step of the XBE build (tools/ui/png2bmp.py), on made-up PNGs and on
  # app/ui/assets/background.png.
  find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
  add_test(NAME ui_png2bmp
    COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/ui/test_png2bmp.py)
  xcat_test_env(ui_png2bmp)

  # The title image the XBE build puts into default.xbe (tools/ui/xbe_title_image.py,
  # docs/formats/xbe-title-image.md), on a small made-up XBE and on app/assets/title-image.png.
  add_test(NAME ui_xbe_title_image
    COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/ui/test_xbe_title_image.py)
  xcat_test_env(ui_xbe_title_image)
endif()
