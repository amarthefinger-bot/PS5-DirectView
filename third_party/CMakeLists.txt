# Vendored Remote Play backend (LAN, input-only) for PS5 DirectView.
#
# chiaki_core = chiaki-ng's protocol library (lib/) built WITHOUT the internet
# Remote Play pieces (holepunch / PSN certificate fetch), which are the only
# parts that need curl, json-c, libevent and miniupnpc. Those entry points are
# replaced by stubs in shims/chiaki_lan_stubs.c that report "not supported".
# No Remote Play video/audio decoding is compiled in (FFmpeg / Opus off).

set(TP "${CMAKE_CURRENT_SOURCE_DIR}")

if(WIN32 AND MSVC)
  set(TP_C_OPTIONS /W0 /utf-8 /MP)
  set(TP_C_DEFS _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS WIN32_LEAN_AND_MEAN
      NOMINMAX _WIN32_WINNT=0x0A00)
else()
  # clang / Apple
  set(TP_C_OPTIONS -w -fno-strict-aliasing)
  set(TP_C_DEFS _DARWIN_C_SOURCE _POSIX_C_SOURCE=200809L)
endif()

# ------------------------------------------------------------------ mbedtls
file(GLOB MBEDTLS_SOURCES CONFIGURE_DEPENDS "${TP}/mbedtls/library/*.c")
add_library(tp_mbedtls STATIC ${MBEDTLS_SOURCES})
target_include_directories(tp_mbedtls PUBLIC "${TP}/mbedtls/include")
target_include_directories(tp_mbedtls PRIVATE "${TP}/mbedtls/library")
target_compile_options(tp_mbedtls PRIVATE ${TP_C_OPTIONS})
target_compile_definitions(tp_mbedtls PRIVATE ${TP_C_DEFS})
if(WIN32)
  target_link_libraries(tp_mbedtls PUBLIC bcrypt advapi32 ws2_32)
endif()

# ------------------------------------------------------------------ nanopb
add_library(tp_nanopb STATIC
  "${TP}/nanopb/pb_common.c" "${TP}/nanopb/pb_encode.c" "${TP}/nanopb/pb_decode.c")
target_include_directories(tp_nanopb PUBLIC "${TP}/nanopb")
target_compile_definitions(tp_nanopb PUBLIC PB_C99_STATIC_ASSERT)
target_compile_options(tp_nanopb PRIVATE ${TP_C_OPTIONS})

# ------------------------------------------------------------------ gf-complete + jerasure
add_library(tp_gf_complete STATIC
  "${TP}/gf-complete/src/gf.c" "${TP}/gf-complete/src/gf_wgen.c"
  "${TP}/gf-complete/src/gf_w4.c" "${TP}/gf-complete/src/gf_w8.c"
  "${TP}/gf-complete/src/gf_w16.c" "${TP}/gf-complete/src/gf_w32.c"
  "${TP}/gf-complete/src/gf_w64.c" "${TP}/gf-complete/src/gf_w128.c"
  "${TP}/gf-complete/src/gf_rand.c" "${TP}/gf-complete/src/gf_general.c"
  "${TP}/gf-complete/src/gf_cpu.c")
target_include_directories(tp_gf_complete PUBLIC "${TP}/gf-complete/include")
target_compile_options(tp_gf_complete PRIVATE ${TP_C_OPTIONS})
target_compile_definitions(tp_gf_complete PRIVATE ${TP_C_DEFS})

add_library(tp_jerasure STATIC
  "${TP}/jerasure/src/galois.c" "${TP}/jerasure/src/jerasure.c"
  "${TP}/jerasure/src/reed_sol.c" "${TP}/jerasure/src/cauchy.c"
  "${TP}/jerasure/src/liberation.c")
target_include_directories(tp_jerasure PUBLIC "${TP}/jerasure/include")
target_link_libraries(tp_jerasure PUBLIC tp_gf_complete)
target_compile_options(tp_jerasure PRIVATE ${TP_C_OPTIONS})
target_compile_definitions(tp_jerasure PRIVATE ${TP_C_DEFS})

# ------------------------------------------------------------------ chiaki core
file(GLOB CHIAKI_SOURCES CONFIGURE_DEPENDS "${TP}/chiaki/src/*.c")
add_library(chiaki_core STATIC
  ${CHIAKI_SOURCES}
  "${TP}/chiaki/generated/takion.pb.c"
  "${TP}/shims/chiaki_lan_stubs.c")
target_include_directories(chiaki_core
  PUBLIC  "${TP}/shims/include" "${TP}/chiaki/include"
  PRIVATE "${TP}/chiaki/generated" "${TP}/chiaki/src")
target_compile_definitions(chiaki_core
  PUBLIC  CHIAKI_LIB_ENABLE_MBEDTLS
  PRIVATE ${TP_C_DEFS} CHIAKI_VERSION_MAJOR=1 CHIAKI_VERSION_MINOR=0 CHIAKI_VERSION_PATCH=0
          CHIAKI_VERSION="directview-lan")
set_property(TARGET chiaki_core PROPERTY C_STANDARD 11)

if(WIN32 AND MSVC)
  # /FI malloc.h: _alloca for the three VLA sites patched in patches/chiaki-msvc-no-vla.patch.
  target_compile_options(chiaki_core PRIVATE ${TP_C_OPTIONS} /FImalloc.h)
  target_link_libraries(chiaki_core PUBLIC tp_mbedtls tp_nanopb tp_jerasure
    ws2_32 wsock32 bcrypt iphlpapi)
elseif(APPLE)
  # Darwin clang: force-include alloca.h and alias MSVC's _alloca to POSIX alloca
  # so the VLA patch (patches/chiaki-msvc-no-vla.patch) compiles unchanged.
  # Also silence chiaki's -Wincompatible-pointer-types / signed-compare noise.
  target_compile_options(chiaki_core PRIVATE ${TP_C_OPTIONS}
    -include alloca.h
    -Wno-incompatible-pointer-types -Wno-pointer-sign -Wno-unused-function
    -Wno-sign-compare -Wno-tautological-pointer-compare
    -Wno-implicit-function-declaration)
  target_compile_definitions(chiaki_core PRIVATE
    _alloca=alloca
    _DARWIN_UNLIMITED_SELECT=1 __APPLE_USE_RFC_3542=1)
  find_library(CHIAKI_FW_CORESERVICES CoreServices REQUIRED)
  target_link_libraries(chiaki_core PUBLIC tp_mbedtls tp_nanopb tp_jerasure
    ${CHIAKI_FW_CORESERVICES})
  # Darwin: no bcrypt/iphlpapi; sockets are in libSystem (no explicit link needed).
endif()
