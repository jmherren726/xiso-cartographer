# Host (PC) platform adapters: file source, directory sink, sockets, clocks and randomness,
# for tests and xcat-cli. Never built for the Xbox (docs/ARCHITECTURE.md §2).
add_library(xcat_host STATIC
  src/host_file.c
  src/host_install.c
  src/host_net.c
  src/host_path.c
  src/host_sink.c
  src/host_tls.c
)
add_library(xcat::host ALIAS xcat_host)
target_include_directories(xcat_host PUBLIC include)
target_link_libraries(xcat_host PUBLIC xcat::xcat)
# Plain CRT stdio (fopen/_wfopen) is intended here; silence the "use *_s" deprecation.
if(WIN32)
  target_compile_definitions(xcat_host PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(xcat_host PUBLIC ws2_32 bcrypt)
else()
  # POSIX functions (mkdir, getaddrinfo, clock_gettime) aren't declared under strict C11 without this.
  target_compile_definitions(xcat_host PRIVATE _POSIX_C_SOURCE=200809L)
endif()
xcat_target_warnings(xcat_host)
