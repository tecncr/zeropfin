cmake_minimum_required(VERSION 3.25)
project(zeropfin VERSION 0.1.0 LANGUAGES C)

# Zeropfin native daemon. See docs/testing.md for the developer workflow:
#   cmake -S . -B build -DZF_SANITIZE=ON && cmake --build build && ctest --test-dir build

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(ZF_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer (host tests)" OFF)
option(ZF_WERROR "Treat compiler warnings as errors" ON)
option(ZF_BUILD_TESTS "Build unit tests, mock player and integration test targets" ON)

# Release metadata (filled by scripts/package.sh / CI from the git tag).
set(ZF_VERSION "development build" CACHE STRING "Version string reported by zeropfin --version")
set(ZF_COMMIT "n/a" CACHE STRING "Git commit reported by zeropfin --version")
string(TIMESTAMP ZF_BUILD_DATE "%Y-%m-%dT%H:%M:%SZ" UTC)
configure_file(src/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MHD REQUIRED IMPORTED_TARGET libmicrohttpd>=0.9.75)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl>=7.74)
pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson>=1.7.14)
find_package(Threads REQUIRED)

set(ZF_WARN -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wformat-truncation=2
            -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wnull-dereference
            -Wdouble-promotion -Wcast-qual -Wwrite-strings)
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
  list(REMOVE_ITEM ZF_WARN -Wformat-truncation=2)
endif()
if(ZF_WERROR)
  list(APPEND ZF_WARN -Werror)
endif()

# C11 language; glibc GNU API extensions (pipe2, accept4, SO_PEERCRED, strcasestr) via _GNU_SOURCE.
add_compile_definitions(_GNU_SOURCE _FILE_OFFSET_BITS=64)
add_compile_options(${ZF_WARN} -fstack-protector-strong)
if(NOT ZF_SANITIZE)
  add_compile_definitions($<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=2>)
endif()

if(ZF_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

# Core library: everything except main(), so tests link the same code the daemon runs.
add_library(zfcore STATIC
  src/util.c
  src/sha256.c
  src/config.c
  src/creds.c
  src/urlutil.c
  src/netfilter.c
  src/auth.c
  src/zp_proto.c
  src/player_state.c
  src/player.c
  src/compat.c
  src/jellyfin.c
  src/reporter.c
  src/relay.c
  src/workers.c
  src/http.c
  src/api.c
  src/ctl.c
  src/setup.c
  src/app.c
)
target_include_directories(zfcore PUBLIC src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(zfcore PUBLIC PkgConfig::MHD PkgConfig::CURL PkgConfig::CJSON Threads::Threads m)

add_executable(zeropfin src/main.c)
target_link_libraries(zeropfin PRIVATE zfcore)
target_link_options(zeropfin PRIVATE -Wl,-z,relro,-z,now -Wl,--as-needed)

install(TARGETS zeropfin RUNTIME DESTINATION bin)

if(ZF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
