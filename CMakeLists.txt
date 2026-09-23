# Cross-platform build. On macOS, ./build_macos.sh does the same without needing CMake.
#   Linux:   sudo apt install libcurl4-openssl-dev && cmake -S . -B build && cmake --build build -j
#   Windows: vcpkg install curl:x64-windows-static
#            cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
#            cmake --build build --config Release
cmake_minimum_required(VERSION 3.16)
project(ComfyRouterOFX VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/utf-8 /EHsc)
  add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

find_package(CURL REQUIRED)

file(GLOB OFX_SUPPORT_SOURCES third_party/openfx/Support/Library/*.cpp)
set(SOURCES
  src/ComfyRouterPlugin.cpp src/RouterClient.cpp src/Media.cpp src/Jobs.cpp src/Settings.cpp
  ${OFX_SUPPORT_SOURCES})
if(APPLE)
  enable_language(OBJCXX)
  list(APPEND SOURCES src/VideoDecode_mac.mm)
  set_source_files_properties(src/VideoDecode_mac.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()

add_library(ComfyRouter MODULE ${SOURCES})
target_include_directories(ComfyRouter PRIVATE
  third_party/openfx/include third_party/openfx/Support/include third_party src)
target_link_libraries(ComfyRouter PRIVATE CURL::libcurl)
set_target_properties(ComfyRouter PROPERTIES PREFIX "" SUFFIX ".ofx")

# OFX bundle layout: ComfyRouter.ofx.bundle/Contents/<arch>/ComfyRouter.ofx
if(WIN32)
  set(OFX_ARCH Win64)
  target_link_libraries(ComfyRouter PRIVATE ws2_32 crypt32 wldap32 normaliz shell32)
elseif(APPLE)
  set(OFX_ARCH MacOS)
  target_link_libraries(ComfyRouter PRIVATE "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo"
                        "-framework Foundation" "-framework CoreFoundation")
  target_link_options(ComfyRouter PRIVATE "-Wl,-exported_symbols_list,${CMAKE_SOURCE_DIR}/resources/exports.txt")
else()
  set(OFX_ARCH Linux-x86-64)
  target_link_libraries(ComfyRouter PRIVATE pthread)
endif()

set(BUNDLE_DIR ${CMAKE_BINARY_DIR}/ComfyRouter.ofx.bundle)
set_target_properties(ComfyRouter PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${BUNDLE_DIR}/Contents/${OFX_ARCH}
  LIBRARY_OUTPUT_DIRECTORY_RELEASE ${BUNDLE_DIR}/Contents/${OFX_ARCH}
  LIBRARY_OUTPUT_DIRECTORY_DEBUG ${BUNDLE_DIR}/Contents/${OFX_ARCH})
if(APPLE)
  configure_file(resources/Info.plist ${BUNDLE_DIR}/Contents/Info.plist COPYONLY)
endif()

# Optional CLI harness (same pipeline, no Resolve needed).
add_executable(router_cli tools/router_cli.cpp src/RouterClient.cpp src/Media.cpp src/Jobs.cpp src/Settings.cpp
               $<$<BOOL:${APPLE}>:src/VideoDecode_mac.mm>)
target_include_directories(router_cli PRIVATE third_party src)
target_link_libraries(router_cli PRIVATE CURL::libcurl)
if(APPLE)
  target_link_libraries(router_cli PRIVATE "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo" "-framework Foundation")
elseif(WIN32)
  target_link_libraries(router_cli PRIVATE ws2_32 crypt32 wldap32 normaliz shell32)
else()
  target_link_libraries(router_cli PRIVATE pthread)
endif()

if(WIN32)
  set(OFX_INSTALL_DIR "C:/Program Files/Common Files/OFX/Plugins" CACHE PATH "OFX plugin folder")
elseif(APPLE)
  set(OFX_INSTALL_DIR "/Library/OFX/Plugins" CACHE PATH "OFX plugin folder")
else()
  set(OFX_INSTALL_DIR "/usr/OFX/Plugins" CACHE PATH "OFX plugin folder")
endif()
install(DIRECTORY ${BUNDLE_DIR} DESTINATION ${OFX_INSTALL_DIR})
