# libxcat: portable core. Must not depend on nxdk or any platform headers.
add_library(xcat
  src/crc32.c
  src/credentials.c
  src/error.c
  src/extract.c
  src/fatx.c
  src/http.c
  src/http_client.c
  src/map.c
  src/source.c
  src/text.c
  src/url.c
  src/version.c
  src/xdvdfs.c
)
add_library(xcat::xcat ALIAS xcat)

target_include_directories(xcat PUBLIC include)
target_compile_features(xcat PUBLIC c_std_11)
xcat_target_warnings(xcat)
