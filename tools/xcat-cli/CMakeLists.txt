# xcat-cli: runs libxcat on a PC (list and extract images).
add_executable(xcat-cli main.c)
target_link_libraries(xcat-cli PRIVATE xcat::xcat xcat::host)
xcat_target_warnings(xcat-cli)
