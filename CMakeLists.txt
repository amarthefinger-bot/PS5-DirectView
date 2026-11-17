cmake_minimum_required(VERSION 3.23)
project(PS5DirectView VERSION 1.1.0 LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "PS5 DirectView targets Windows 10/11 only (Win32, Media Foundation, Direct3D 11).")
endif()
if(NOT MSVC)
  message(FATAL_ERROR "Build with the Visual Studio 2022 MSVC x64 toolchain.")
endif()

# Static CRT for the app and every vendored library (single self-contained exe).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Remote Play input backend (chiaki-ng core, LAN only, no A/V decoding).
add_subdirectory(third_party EXCLUDE_FROM_ALL)

add_executable(PS5DirectView WIN32
  src/main.cpp
  src/Capture.cpp
  src/Capture.h
  src/Renderer.cpp
  src/Renderer.h
  src/Audio.cpp
  src/Audio.h
  src/Ui.cpp
  src/Ui.h
  src/ControllerState.cpp
  src/ControllerState.h
  src/InputSettings.cpp
  src/InputSettings.h
  src/MouseTranslator.cpp
  src/MouseTranslator.h
  src/InputManager.cpp
  src/InputManager.h
  src/RemotePlayClient.cpp
  src/RemotePlayClient.h
  src/SecureStore.cpp
  src/SecureStore.h
)
target_compile_features(PS5DirectView PRIVATE cxx_std_20)
target_compile_definitions(PS5DirectView PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
)
target_compile_options(PS5DirectView PRIVATE /W4 /EHsc /utf-8 /permissive-)
target_link_libraries(PS5DirectView PRIVATE
  chiaki_core
  mfplat mfreadwrite mf mfuuid
  d3d11 dxgi d3dcompiler d2d1 dwrite windowscodecs dwmapi
  ole32 oleaut32 user32 gdi32 comdlg32 advapi32 avrt shell32 cfgmgr32 crypt32 winmm
)
set_target_properties(PS5DirectView PROPERTIES
  OUTPUT_NAME "PS5DirectView"
  VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:PS5DirectView>"
)

# Offline unit tests for the mouse/keyboard -> controller translation (no PS5 needed).
add_executable(InputTests EXCLUDE_FROM_ALL
  tests/InputTests.cpp
  src/ControllerState.cpp
  src/InputSettings.cpp
  src/MouseTranslator.cpp
)
target_include_directories(InputTests PRIVATE src)
target_compile_features(InputTests PRIVATE cxx_std_20)
target_compile_definitions(InputTests PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(InputTests PRIVATE /W4 /EHsc /utf-8 /permissive-)
target_link_libraries(InputTests PRIVATE advapi32 user32)

# Offline Remote Play crypto regression tests (pairing / handshake code paths).
add_executable(CryptoTests EXCLUDE_FROM_ALL tests/CryptoTests.cpp)
target_compile_features(CryptoTests PRIVATE cxx_std_20)
target_compile_options(CryptoTests PRIVATE /EHsc /utf-8)
target_link_libraries(CryptoTests PRIVATE chiaki_core)

# Read-only LAN check of PS5 discovery (needs the console on the network).
add_executable(RemotePlayProbe EXCLUDE_FROM_ALL
  tests/RemotePlayProbe.cpp
  src/RemotePlayClient.cpp
  src/SecureStore.cpp
  src/ControllerState.cpp
)
target_include_directories(RemotePlayProbe PRIVATE src)
target_compile_features(RemotePlayProbe PRIVATE cxx_std_20)
target_compile_definitions(RemotePlayProbe PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(RemotePlayProbe PRIVATE /W4 /EHsc /utf-8 /permissive-)
target_link_libraries(RemotePlayProbe PRIVATE chiaki_core crypt32 advapi32 user32)
