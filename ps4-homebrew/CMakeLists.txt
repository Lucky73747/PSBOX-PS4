# Isolated PS4 homebrew bootstrap for the Xbox Cloud UI.
# Configure with the paired OpenOrbis/Mesa SDK bundle and its porting kit.
cmake_minimum_required(VERSION 3.20)
set(_xc_repo_root "${CMAKE_CURRENT_LIST_DIR}/..")
file(STRINGS "${_xc_repo_root}/VERSION" PSBOX_VERSION LIMIT_COUNT 1)
file(STRINGS "${_xc_repo_root}/BUILD_NUMBER" PSBOX_BUILD_NUMBER_DEFAULT LIMIT_COUNT 1)
if(NOT PSBOX_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "VERSION must contain a semantic version such as 0.1.0")
endif()
if(NOT PSBOX_BUILD_NUMBER_DEFAULT MATCHES "^[0-9]+$")
  message(FATAL_ERROR "BUILD_NUMBER must contain a numeric build number")
endif()
project(psbox_ps4 VERSION "${PSBOX_VERSION}" LANGUAGES C CXX)
set(PSBOX_BUILD_NUMBER "${PSBOX_BUILD_NUMBER_DEFAULT}" CACHE STRING "PSBOX source build number")
set(PSBOX_LIBRARY_LAYOUT "NEW" CACHE STRING "Default library UI layout: NEW or LEGACY")
set_property(CACHE PSBOX_LIBRARY_LAYOUT PROPERTY STRINGS NEW LEGACY)
string(TOUPPER "${PSBOX_LIBRARY_LAYOUT}" PSBOX_LIBRARY_LAYOUT)
if(NOT PSBOX_LIBRARY_LAYOUT STREQUAL "NEW" AND NOT PSBOX_LIBRARY_LAYOUT STREQUAL "LEGACY")
  message(FATAL_ERROR "PSBOX_LIBRARY_LAYOUT must be NEW or LEGACY")
endif()
if(NOT PSBOX_BUILD_NUMBER MATCHES "^[0-9]+$")
  message(FATAL_ERROR "PSBOX_BUILD_NUMBER must be numeric")
endif()
option(PSBOX_EXPERIMENTAL_LINK_GC_SECTIONS
  "Discard unreferenced code/data sections from the statically linked PS4 app" OFF)

if(NOT PS4 OR NOT ORBIS_COMPAT_DIR)
  message(FATAL_ERROR "Use the paired orbis-sdk.cmake toolchain; this target must not build as a host app")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(PREPEND CMAKE_MODULE_PATH "${_xc_repo_root}/cmake/modules")

# Opus is built static before libdatachannel enables BUILD_SHARED_LIBS globally.
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_DRED OFF CACHE BOOL "" FORCE)
set(OPUS_OSCE OFF CACHE BOOL "" FORCE)
set(_xc_build_shared_libs_before_opus ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
add_subdirectory("${_xc_repo_root}/deps/opus" "${CMAKE_BINARY_DIR}/opus" EXCLUDE_FROM_ALL)
set(BUILD_SHARED_LIBS ${_xc_build_shared_libs_before_opus})

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
set(LIBSRTP_TEST_APPS OFF CACHE BOOL "" FORCE)
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
  stream_menu_ui.cpp
  stream_stats_ui.cpp
  audio_player.cpp
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
  system_keyboard.cpp
  ../extern/qrcodegen/qrcodegen.c)
target_include_directories(xcloud_homebrew PRIVATE
  ../src
  "${XC_PS4_SDL3_SOURCE}/include"
  "${XC_PS4_SDL3_BUILD}/include-config-release"
  "${XC_PS4_SDL3_BUILD}/include-revision"
  ../extern/stb
  ../extern/qrcodegen
  "${_xc_repo_root}/deps/opus/include"
  "${_vkloader_dir}"
  "${_xc_repo_root}/deps/libdatachannel/include"
  "${ORBIS_MESA_SRC}/include")
target_compile_definitions(xcloud_homebrew PRIVATE
  "PSBOX_BUILD_LABEL=\"PSBOX-PS4-v${PROJECT_VERSION}-build${PSBOX_BUILD_NUMBER}\"")
if(PSBOX_LIBRARY_LAYOUT STREQUAL "NEW")
  target_compile_definitions(xcloud_homebrew PRIVATE PSBOX_DEFAULT_NEW_LIBRARY_UI=1)
else()
  target_compile_definitions(xcloud_homebrew PRIVATE PSBOX_DEFAULT_NEW_LIBRARY_UI=0)
endif()
# Keep the RADV archive whole so the linked ICD retains its entry points. The
# explicit -Wl form is required by the Clang 21 driver in this workspace.
target_link_options(xcloud_homebrew PRIVATE "-Wl,--wrap=sceVideoOutSubmitFlip")
if(PSBOX_EXPERIMENTAL_LINK_GC_SECTIONS)
  target_link_options(xcloud_homebrew PRIVATE "-Wl,--gc-sections")
endif()
target_link_libraries(xcloud_homebrew PRIVATE
  -Wl,--whole-archive "${_radv_archive}" -Wl,--no-whole-archive
  "${XC_PS4_SDL3_BUILD}/libSDL3.a"
  ${_zlib_archive}
  datachannel-static
  opus
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
  configure_file("${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/OFL.txt" "${_assets}/fonts/OFL.txt" COPYONLY)
  file(MAKE_DIRECTORY "${_assets}/licenses")
  configure_file("${CMAKE_CURRENT_LIST_DIR}/../LICENSE" "${_assets}/licenses/LICENSE" COPYONLY)
  configure_file("${CMAKE_CURRENT_LIST_DIR}/../THIRD_PARTY_NOTICES.md" "${_assets}/licenses/THIRD_PARTY_NOTICES.md" COPYONLY)
  configure_file("${CMAKE_CURRENT_LIST_DIR}/../extern/fsr1/LICENSE.txt" "${_assets}/licenses/AMD-FSR1.txt" COPYONLY)
  configure_file("${_xc_repo_root}/deps/opus/COPYING" "${_assets}/licenses/Opus-COPYING" COPYONLY)
  # Retain the public notice collection in every newly generated package.
  # This does not replace the corresponding-source/external SDK review.
  file(GLOB_RECURSE _xc_public_notices CONFIGURE_DEPENDS
    RELATIVE "${_xc_repo_root}/licenses" "${_xc_repo_root}/licenses/*")
  set(_xc_notice_extra_files)
  foreach(_notice IN LISTS _xc_public_notices)
    if(NOT IS_DIRECTORY "${_xc_repo_root}/licenses/${_notice}")
      get_filename_component(_notice_dir "${_notice}" DIRECTORY)
      file(MAKE_DIRECTORY "${_assets}/licenses/third-party/${_notice_dir}")
      configure_file("${_xc_repo_root}/licenses/${_notice}"
        "${_assets}/licenses/third-party/${_notice}" COPYONLY)
      list(APPEND _xc_notice_extra_files
        "${_assets}/licenses/third-party/${_notice}:licenses/third-party/${_notice}")
    endif()
  endforeach()
  ps4_create_pkg(xcloud_homebrew
    TITLE_ID PSBX00001
    TITLE "PSBOX"
    VERSION 00.03
    CONTENT_LABEL PSBOXPS4
    ICON "${CMAKE_CURRENT_LIST_DIR}/../assets/ps4/icon0.png"
    EXTRA_FILES
      "${_assets}/fonts/Inter-Regular.ttf:assets/fonts/Inter-Regular.ttf"
      "${_assets}/fonts/Inter-SemiBold.ttf:assets/fonts/Inter-SemiBold.ttf"
      "${_assets}/fonts/Inter-Bold.ttf:assets/fonts/Inter-Bold.ttf"
      "${_assets}/fonts/OFL.txt:assets/fonts/OFL.txt"
      "${_assets}/licenses/AMD-FSR1.txt:licenses/AMD-FSR1.txt"
      "${_assets}/licenses/LICENSE:licenses/LICENSE"
      "${_assets}/licenses/Opus-COPYING:licenses/Opus-COPYING"
      "${_assets}/licenses/THIRD_PARTY_NOTICES.md:licenses/THIRD_PARTY_NOTICES.md"
      ${_xc_notice_extra_files})
endif()
