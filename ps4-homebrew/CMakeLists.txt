# Isolated PS4 homebrew bootstrap for the Xbox Cloud UI.
# Configure with the paired OpenOrbis/Mesa SDK bundle and its porting kit.
cmake_minimum_required(VERSION 3.20)

if(NOT PS4 OR NOT ORBIS_COMPAT_DIR)
  message(FATAL_ERROR "Use the paired orbis-sdk.cmake toolchain; this target must not build as a host app")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(PREPEND CMAKE_MODULE_PATH "${_xc_repo_root}/cmake/modules")

# Build the existing WebRTC client and its pinned crypto/ICE/SCTP dependencies
# for Orbis. The standalone bootstrap intentionally did not pull these into
# its link graph before the video transport milestone.
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL OFF CACHE BOOL "" FORCE)
set(MBEDTLS_USER_CONFIG_FILE "${_xc_repo_root}/cmake/deps/mbedtls_user_config.h" CACHE FILEPATH "" FORCE)
add_subdirectory("${_xc_repo_root}/deps/mbedtls" "${CMAKE_BINARY_DIR}/mbedtls" EXCLUDE_FROM_ALL)
add_library(xc_mbedtls INTERFACE)
target_link_libraries(xc_mbedtls INTERFACE mbedtls mbedx509 mbedcrypto)
target_include_directories(xc_mbedtls INTERFACE "${_xc_repo_root}/deps/mbedtls/include")
add_library(MbedTLS::MbedTLS INTERFACE IMPORTED GLOBAL)
set_target_properties(MbedTLS::MbedTLS PROPERTIES
  INTERFACE_LINK_LIBRARIES "mbedtls;mbedx509;mbedcrypto")

set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_DEPS_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_WITH_WARNINGS OFF CACHE BOOL "" FORCE)
# The Orbis cross sysroot contains a few compatibility headers with names that
# make usrsctp's configure checks mistake it for native Linux/FreeBSD userspace.
# Keep its portable bundled queue implementation for this PS4 userspace build.
set(have_sys_queue_h 0 CACHE INTERNAL "Orbis does not provide compatible sys/queue.h" FORCE)
set(have_linux_if_addr_h 0 CACHE INTERNAL "Not a Linux target" FORCE)
set(have_linux_rtnetlink_h 0 CACHE INTERNAL "Not a Linux target" FORCE)
add_subdirectory("${_xc_repo_root}/deps/libdatachannel"
  "${CMAKE_BINARY_DIR}/libdatachannel" EXCLUDE_FROM_ALL)

# usrsctp's C target detects sockaddr_conn.sconn_len and is compiled with
# HAVE_SCONN_LEN. The libc++ Orbis headers undefine __FreeBSD__, so the
# libdatachannel C++ target otherwise sees the non-BSD sockaddr_conn layout
# from usrsctp.h (family at offset 0 instead of offset 1). Keep both targets
# ABI-compatible with the bundled usrsctp build.
target_compile_definitions(datachannel PRIVATE HAVE_SCONN_LEN)
target_compile_definitions(datachannel-static PRIVATE HAVE_SCONN_LEN)

set(ORBIS_KIT_DIR "${_xc_repo_root}/dependencies/orbis-porting-kit" CACHE PATH
  "orbis-porting-kit checkout containing vkloader/")
if(NOT ORBIS_KIT_DIR OR NOT EXISTS "${ORBIS_KIT_DIR}/vkloader/CMakeLists.txt")
  message(FATAL_ERROR "Set ORBIS_KIT_DIR to the existing orbis-porting-kit checkout")
endif()

set(XC_PS4_SDL3_SOURCE "${_xc_repo_root}/dependencies/SDL" CACHE PATH
  "SDL3 source tree used by the PS4 port")
set(XC_PS4_SDL3_BUILD "${_xc_repo_root}/dependencies/SDL-build-sdl3-freebsd" CACHE PATH
  "Existing SDL3 PS4 build tree")
if(NOT EXISTS "${XC_PS4_SDL3_SOURCE}/include/SDL3/SDL.h" OR
   NOT EXISTS "${XC_PS4_SDL3_BUILD}/libSDL3.a")
  message(FATAL_ERROR "Set XC_PS4_SDL3_SOURCE/BUILD to the existing SDL3 PS4 port and archive")
endif()

set(_vkloader_dir "${ORBIS_KIT_DIR}/vkloader")
set(_radv_archive "${ORBIS_MESA_BUILD}/src/amd/vulkan/libvulkan_radeon.a")
file(GLOB _zlib_archive "${ORBIS_MESA_BUILD}/subprojects/zlib-*/libz.a")
if(NOT EXISTS "${_radv_archive}" OR NOT _zlib_archive)
  message(FATAL_ERROR "The paired Mesa RADV/zlib archives are missing under ORBIS_MESA_BUILD=${ORBIS_MESA_BUILD}")
endif()

add_executable(xcloud_homebrew
  main.cpp
  gpu_wsi.cpp
  video_decoder.cpp
  random_compat.c
  "${ORBIS_COMPAT_DIR}/optional/orbis_thread_atexit_stub.c"
  network.cpp
  net_http.cpp
  xcloud_login.cpp
  "${_vkloader_dir}/vkloader.c"
  "${_vkloader_dir}/vkthunks.c"
  ../src/ui/canvas.cpp
  ../src/ui/font.cpp
  ../src/ui/brand.cpp
  ../src/ui/app_ui.cpp
  ../src/ui/image_cache.cpp
  ../src/ui/accent_color.cpp
  ../src/ui/strings.cpp
  ../src/ui/pad_icons.cpp
  ../src/app/settings.cpp
  ../src/util/log.cpp
  ../src/util/json.cpp
  ../src/auth/msa.cpp
  ../src/auth/xbox_live.cpp
  ../src/xcloud/gssv.cpp
  ../src/xcloud/catalog.cpp
  ../src/stream/stream_session.cpp
  ../src/stream/h264_receiver.cpp
  ../src/stream/rtcp_reporter.cpp
  ../src/stream/input_packet.cpp
  ../src/platform/ps4_homebrew_clock.cpp
  ../src/platform/ps4_homebrew_file.cpp
  ../extern/qrcodegen/qrcodegen.c)
target_include_directories(xcloud_homebrew PRIVATE
  ../src
  "${XC_PS4_SDL3_SOURCE}/include"
  "${XC_PS4_SDL3_BUILD}/include-config-release"
  "${XC_PS4_SDL3_BUILD}/include-revision"
  ../extern/stb
  ../extern/qrcodegen
  "${_vkloader_dir}"
  "${_xc_repo_root}/deps/libdatachannel/include"
  "${ORBIS_MESA_SRC}/include")
# Keep the RADV archive whole so the linked ICD retains its entry points. The
# explicit -Wl form is required by the Clang 21 driver in this workspace.
target_link_libraries(xcloud_homebrew PRIVATE
  -Wl,--whole-archive "${_radv_archive}" -Wl,--no-whole-archive
  "${XC_PS4_SDL3_BUILD}/libSDL3.a"
  ${_zlib_archive}
  datachannel-static
  -lSceGnmDriver -lSceVideoOut -lSceSysmodule -lSceUserService -lScePad -lSceAudioOut
  -lSceNet -lSceNetCtl -lSceSsl -lSceHttp -lSceRandom)

if(COMMAND ps4_create_eboot)
  ps4_create_eboot(xcloud_homebrew)
endif()

if(COMMAND ps4_create_pkg)
  set(_assets "${CMAKE_CURRENT_BINARY_DIR}/pkg-assets")
  file(MAKE_DIRECTORY "${_assets}/fonts")
  foreach(_font Inter-Regular.ttf Inter-SemiBold.ttf Inter-Bold.ttf)
    configure_file("${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/${_font}" "${_assets}/fonts/${_font}" COPYONLY)
  endforeach()
  ps4_create_pkg(xcloud_homebrew
    TITLE_ID XCHM00001
    EXTRA_FILES
      "${_assets}/fonts/Inter-Regular.ttf:assets/fonts/Inter-Regular.ttf"
      "${_assets}/fonts/Inter-SemiBold.ttf:assets/fonts/Inter-SemiBold.ttf"
endif()
