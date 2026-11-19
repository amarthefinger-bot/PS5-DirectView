cmake_minimum_required(VERSION 3.23)
project(PS5DirectView VERSION 1.3.0 LANGUAGES C CXX)

if(APPLE)
  enable_language(OBJC)
  enable_language(OBJCXX)
endif()

if(WIN32)
  set(PS5DV_WIN TRUE)
elseif(APPLE)
  set(PS5DV_MAC TRUE)
else()
  message(FATAL_ERROR "PS5 DirectView targets Windows 10/11 or macOS 12+.")
endif()

if(PS5DV_WIN AND NOT MSVC)
  message(FATAL_ERROR "On Windows build with the Visual Studio 2022 MSVC x64 toolchain.")
endif()

# Static CRT on Windows for a single self-contained exe.
if(PS5DV_WIN)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# macOS: target modern Apple frameworks; universal build (arm64 + x86_64).
if(PS5DV_MAC)
  if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version" FORCE)
  endif()
  if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal build" FORCE)
  endif()
endif()

# Remote Play input backend (chiaki-ng core, LAN only, no A/V decoding).
add_subdirectory(third_party EXCLUDE_FROM_ALL)

# Portable sources shared across platforms.
set(PS5DV_SHARED_SRC
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

if(PS5DV_WIN)
  add_executable(PS5DirectView WIN32 ${PS5DV_SHARED_SRC})
  target_compile_definitions(PS5DirectView PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
  target_compile_options(PS5DirectView PRIVATE /W4 /EHsc /utf-8 /permissive-)
  target_link_libraries(PS5DirectView PRIVATE
    chiaki_core
    mfplat mfreadwrite mf mfuuid
    d3d11 dxgi d3dcompiler d2d1 dwrite windowscodecs dwmapi
    ole32 oleaut32 user32 gdi32 comdlg32 advapi32 avrt shell32 cfgmgr32 crypt32 winmm
  )
  set_target_properties(PS5DirectView PROPERTIES
    OUTPUT_NAME "PS5DirectView"
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:PS5DirectView>")
endif()

if(PS5DV_MAC)
  set(PS5DV_MAC_SRC
    src/mac/MacEntry.mm
    src/mac/Capture_Mac.mm
    src/mac/Renderer_Mac.mm
    src/mac/Audio_Mac.mm
    src/mac/InputManager_Mac.mm
    src/mac/Ui_Mac.mm
    src/mac/SecureStore_Mac.mm
    src/mac/RemotePlayClient_Mac.cpp
    src/mac/MacApp.h
    src/mac/MacApp.mm
  )

  add_executable(PS5DirectView MACOSX_BUNDLE ${PS5DV_SHARED_SRC} ${PS5DV_MAC_SRC})
  target_compile_features(PS5DirectView PRIVATE cxx_std_20)
  target_compile_options(PS5DirectView PRIVATE
    -Wall -Wno-unused-parameter -Wno-deprecated-declarations)
  foreach(_src IN LISTS PS5DV_MAC_SRC)
    if(_src MATCHES "\\.mm$")
      set_source_files_properties(${_src} PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    endif()
  endforeach()
  target_include_directories(PS5DirectView PRIVATE src src/mac)

  find_library(FW_COCOA Cocoa REQUIRED)
  find_library(FW_METAL Metal REQUIRED)
  find_library(FW_METALKIT MetalKit REQUIRED)
  find_library(FW_QUARTZCORE QuartzCore REQUIRED)
  find_library(FW_COREVIDEO CoreVideo REQUIRED)
  find_library(FW_COREMEDIA CoreMedia REQUIRED)
  find_library(FW_COREMEDIAIO CoreMediaIO REQUIRED)
  find_library(FW_AVFOUNDATION AVFoundation REQUIRED)
  find_library(FW_COREAUDIO CoreAudio REQUIRED)
  find_library(FW_AUDIOTOOLBOX AudioToolbox REQUIRED)
  find_library(FW_AUDIOUNIT AudioUnit)
  find_library(FW_IOKIT IOKit REQUIRED)
  find_library(FW_CORETEXT CoreText REQUIRED)
  find_library(FW_COREGRAPHICS CoreGraphics REQUIRED)
  find_library(FW_IMAGEIO ImageIO REQUIRED)
  find_library(FW_APPKIT AppKit REQUIRED)
  find_library(FW_FOUNDATION Foundation REQUIRED)
  find_library(FW_SECURITY Security REQUIRED)
  find_library(FW_SYSTEMCONFIG SystemConfiguration REQUIRED)
  find_library(FW_VIDEOTOOLBOX VideoToolbox REQUIRED)

  target_link_libraries(PS5DirectView PRIVATE
    chiaki_core
    ${FW_COCOA} ${FW_METAL} ${FW_METALKIT} ${FW_QUARTZCORE}
    ${FW_COREVIDEO} ${FW_COREMEDIA} ${FW_COREMEDIAIO} ${FW_AVFOUNDATION}
    ${FW_COREAUDIO} ${FW_AUDIOTOOLBOX} ${FW_AUDIOUNIT}
    ${FW_IOKIT} ${FW_CORETEXT} ${FW_COREGRAPHICS} ${FW_IMAGEIO}
    ${FW_APPKIT} ${FW_FOUNDATION} ${FW_SECURITY}
    ${FW_SYSTEMCONFIG} ${FW_VIDEOTOOLBOX}
  )

  set_target_properties(PS5DirectView PROPERTIES
    OUTPUT_NAME "PS5DirectView"
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/src/mac/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.ps5directview.app"
    MACOSX_BUNDLE_BUNDLE_NAME "PS5 DirectView"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_COPYRIGHT "MIT"
  )
endif()

target_compile_features(PS5DirectView PRIVATE cxx_std_20)

# Offline unit tests (portable on both platforms).
add_executable(InputTests EXCLUDE_FROM_ALL
  tests/InputTests.cpp
  src/ControllerState.cpp
  src/InputSettings.cpp
  src/MouseTranslator.cpp
)
target_include_directories(InputTests PRIVATE src)
target_compile_features(InputTests PRIVATE cxx_std_20)
if(PS5DV_WIN)
  target_compile_definitions(InputTests PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
  target_compile_options(InputTests PRIVATE /W4 /EHsc /utf-8 /permissive-)
  target_link_libraries(InputTests PRIVATE advapi32 user32)
endif()

add_executable(CryptoTests EXCLUDE_FROM_ALL tests/CryptoTests.cpp)
target_compile_features(CryptoTests PRIVATE cxx_std_20)
if(PS5DV_WIN)
  target_compile_options(CryptoTests PRIVATE /EHsc /utf-8)
endif()
target_link_libraries(CryptoTests PRIVATE chiaki_core)

if(PS5DV_WIN)
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
endif()

add_executable(FrameGenerationTests EXCLUDE_FROM_ALL tests/FrameGenerationTests.cpp)
target_include_directories(FrameGenerationTests PRIVATE src)
target_compile_features(FrameGenerationTests PRIVATE cxx_std_20)

if(PS5DV_WIN)
  add_executable(RendererFrameGenerationTests EXCLUDE_FROM_ALL
    tests/RendererFrameGenerationTests.cpp src/Renderer.cpp)
  target_include_directories(RendererFrameGenerationTests PRIVATE src)
  target_compile_features(RendererFrameGenerationTests PRIVATE cxx_std_20)
  target_compile_definitions(RendererFrameGenerationTests PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
  target_compile_options(RendererFrameGenerationTests PRIVATE /EHsc /utf-8)
  target_link_libraries(RendererFrameGenerationTests PRIVATE
    mfplat mfreadwrite mf mfuuid d3d11 dxgi d3dcompiler d2d1 dwrite
    windowscodecs ole32 user32 gdi32)
endif()
