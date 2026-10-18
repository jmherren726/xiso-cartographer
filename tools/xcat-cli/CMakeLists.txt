# xcat-cli: runs libxcat on a PC (list and extract images).
add_executable(xcat-cli main.c)
target_link_libraries(xcat-cli PRIVATE xcat::xcat xcat::host)
xcat_target_warnings(xcat-cli)
if(NOT WIN32)
  # gmtime_r (xcat-cli ntp) isn't declared under strict C11 without this.
  target_compile_definitions(xcat-cli PRIVATE _POSIX_C_SOURCE=200809L)
endif()
