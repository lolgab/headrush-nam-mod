cmake_minimum_required(VERSION 3.16)
project(headrush_nam_gui C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_library(nam_gui_core STATIC
  core/model_targets.c
  core/fit_image.c
  core/elf_patch.c
  core/qml_patch.c
  core/ext4_image.c
  core/xz_codec.c
  core/launcher_script.c
  core/patch_pipeline.c
  core/http_download.c
  core/zip_reader.c
  core/tempdir.c
  third_party/sha1/sha1.c
  third_party/miniz/miniz.c
  third_party/miniz/miniz_tdef.c
  third_party/miniz/miniz_tinfl.c
  third_party/miniz/miniz_zip.c
)
target_include_directories(nam_gui_core PUBLIC core third_party/sha1 third_party/miniz)

find_path(CURL_INCLUDE_DIR curl/curl.h
  PATHS /opt/homebrew/opt/curl /usr/local/opt/curl /usr
  PATH_SUFFIXES include
)
find_library(CURL_LIBRARY
  NAMES curl
  PATHS /opt/homebrew/opt/curl /usr/local/opt/curl /usr
  PATH_SUFFIXES lib
)
if(NOT CURL_INCLUDE_DIR OR NOT CURL_LIBRARY)
  message(FATAL_ERROR "libcurl not found -- macOS: brew install curl; Debian/Ubuntu: apt install libcurl4-openssl-dev.")
endif()
target_include_directories(nam_gui_core PUBLIC ${CURL_INCLUDE_DIR})
target_link_libraries(nam_gui_core PUBLIC ${CURL_LIBRARY})

find_path(LZMA_INCLUDE_DIR lzma.h
  PATHS /opt/homebrew/opt/xz /usr/local/opt/xz /usr
  PATH_SUFFIXES include
)
find_library(LZMA_LIBRARY
  NAMES lzma
  PATHS /opt/homebrew/opt/xz /usr/local/opt/xz /usr
  PATH_SUFFIXES lib
)
if(NOT LZMA_INCLUDE_DIR OR NOT LZMA_LIBRARY)
  message(FATAL_ERROR "liblzma (xz) not found -- macOS: brew install xz; Debian/Ubuntu: apt install liblzma-dev.")
endif()
target_include_directories(nam_gui_core PUBLIC ${LZMA_INCLUDE_DIR})
target_link_libraries(nam_gui_core PUBLIC ${LZMA_LIBRARY})

# e2fsprogs (libext2fs) -- replaces the debugfs/e2fsck CLI shell-outs
# build_update_img.py uses.
if(WIN32)
  # No vcpkg or MSYS2/MinGW binary package for libext2fs exists (checked
  # both indexes) -- but the e2fsprogs SOURCE builds cleanly for mingw-w64
  # (confirmed by actually building it, not just reading configure.ac,
  # which already special-cases `mingw*` hosts -- see
  # core/ext4_image.c's use of default_io_manager instead of a hardcoded
  # unix_io_manager). Built once per fresh checkout via
  # gui/scripts/build_e2fsprogs_windows.sh, same "build on the fly, never
  # commit" philosophy as gui/blobs/* above. Requires a bash + a C
  # compiler capable of building for mingw-w64 (e.g. run this from an
  # MSYS2 MinGW64 shell, which provides both natively).
  set(E2FSPROGS_WIN_PREFIX ${CMAKE_SOURCE_DIR}/build/e2fsprogs-windows)
  if(NOT EXISTS ${E2FSPROGS_WIN_PREFIX}/lib/libext2fs.a)
    message(STATUS "Building e2fsprogs libraries for Windows (mingw-w64) -- this takes a few minutes...")
    execute_process(
      COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/build_e2fsprogs_windows.sh ${E2FSPROGS_WIN_PREFIX}
      RESULT_VARIABLE E2FSPROGS_WIN_BUILD_RESULT
    )
    if(NOT E2FSPROGS_WIN_BUILD_RESULT EQUAL 0)
      message(FATAL_ERROR "Building e2fsprogs for Windows failed -- see gui/scripts/build_e2fsprogs_windows.sh "
                           "(needs bash + a mingw-w64-capable C compiler, e.g. an MSYS2 MinGW64 shell).")
    endif()
  endif()
  set(E2FSPROGS_INCLUDE_DIR ${E2FSPROGS_WIN_PREFIX}/include)
  set(E2FSPROGS_LIBRARIES
    ${E2FSPROGS_WIN_PREFIX}/lib/libext2fs.a
    ${E2FSPROGS_WIN_PREFIX}/lib/libe2p.a
    ${E2FSPROGS_WIN_PREFIX}/lib/libcom_err.a
    ${E2FSPROGS_WIN_PREFIX}/lib/libblkid.a
    ${E2FSPROGS_WIN_PREFIX}/lib/libuuid.a
  )
else()
  # No .pc file on Homebrew, so search common keg/system locations
  # directly; override with -DE2FSPROGS_ROOT=... if yours lives elsewhere
  # (e.g. a Linux distro package, or the docker/ image).
  find_path(E2FSPROGS_INCLUDE_DIR ext2fs/ext2fs.h
    HINTS ${E2FSPROGS_ROOT}
    PATHS /opt/homebrew/opt/e2fsprogs /usr/local/opt/e2fsprogs /usr
    PATH_SUFFIXES include
  )
  foreach(_lib ext2fs e2p com_err blkid uuid)
    find_library(E2FSPROGS_${_lib}_LIBRARY
      NAMES ${_lib}
      HINTS ${E2FSPROGS_ROOT}
      PATHS /opt/homebrew/opt/e2fsprogs /usr/local/opt/e2fsprogs /usr
      PATH_SUFFIXES lib
    )
    list(APPEND E2FSPROGS_LIBRARIES ${E2FSPROGS_${_lib}_LIBRARY})
  endforeach()

  if(NOT E2FSPROGS_INCLUDE_DIR OR NOT E2FSPROGS_ext2fs_LIBRARY)
    message(FATAL_ERROR "libext2fs (e2fsprogs) not found -- macOS: brew install e2fsprogs; "
                         "Debian/Ubuntu: apt install e2fslibs-dev (or e2fsprogs-dev/uuid-dev as "
                         "needed for your distro); or pass -DE2FSPROGS_ROOT=/path/to/e2fsprogs.")
  endif()
endif()

target_include_directories(nam_gui_core PUBLIC ${E2FSPROGS_INCLUDE_DIR})
target_link_libraries(nam_gui_core PUBLIC ${E2FSPROGS_LIBRARIES})

# gui/blobs/{libnam_hook.so,libnam_preload.so,trampoline_gonk.bin} are pure
# build ARTIFACTS -- deliberately NOT committed to git (see .gitignore).
# They're a fixed function of patch/*.cpp + nam_core (never of user input),
# so this target rebuilds them via Docker (gui/scripts/build_blobs.sh, the
# same ARM cross-toolchain scripts/build_docker.sh uses) whenever their
# real inputs change, and every build depends on it -- nothing here ever
# trusts a possibly-stale/missing checked-in binary.
file(GLOB NAM_HOOK_SOURCES
  ${CMAKE_SOURCE_DIR}/../patch/nam_hook.cpp
  ${CMAKE_SOURCE_DIR}/../patch/nam_preload.cpp
  ${CMAKE_SOURCE_DIR}/../patch/trampoline_gonk.S
)
file(GLOB_RECURSE NAM_CORE_SOURCES
  ${CMAKE_SOURCE_DIR}/../nam_core/NAM/*.cpp
  ${CMAKE_SOURCE_DIR}/../nam_core/NAM/*.h
)
set(NAM_BLOBS
  ${CMAKE_SOURCE_DIR}/blobs/libnam_hook.so
  ${CMAKE_SOURCE_DIR}/blobs/libnam_preload.so
  ${CMAKE_SOURCE_DIR}/blobs/trampoline_gonk.bin
)
add_custom_command(
  OUTPUT ${NAM_BLOBS}
  COMMAND ${CMAKE_SOURCE_DIR}/scripts/build_blobs.sh
  DEPENDS ${NAM_HOOK_SOURCES} ${NAM_CORE_SOURCES} ${CMAKE_SOURCE_DIR}/../docker/Dockerfile
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/..
  COMMENT "Rebuilding gui/blobs/* from patch/*.cpp via the ARM cross-toolchain Docker image (needs Docker running)"
  VERBATIM
)
add_custom_target(nam_gui_blobs ALL DEPENDS ${NAM_BLOBS})

# gui/core CLI (milestone 1 -- no GUI yet). POSIX-only for now (mkdtemp) --
# Windows support is a later packaging milestone.
add_executable(gui-core-cli tools/gui_core_cli.c)
target_link_libraries(gui-core-cli PRIVATE nam_gui_core)
add_dependencies(gui-core-cli nam_gui_blobs)

# Milestone 3: Nuklear (public domain, vendored header) + SDL2 GUI wrapping
# nam_gui_core. Linux/simplest-output-path only for now (no .app/.exe
# repacking yet -- see the plan's milestones 4/5).
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
  find_path(SDL2_INCLUDE_DIRS SDL.h PATH_SUFFIXES SDL2
    PATHS /opt/homebrew/include /usr/local/include /usr/include)
  find_library(SDL2_LIBRARIES NAMES SDL2
    PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)
endif()

if(SDL2_INCLUDE_DIRS AND SDL2_LIBRARIES)
  set(GUI_APP_SOURCES app/main.c)
  if(APPLE)
    # Milestone 4: repack the whole downloaded .app + ad-hoc codesign --
    # shells out to `cp`/`codesign`, both always present on macOS.
    list(APPEND GUI_APP_SOURCES app/mac_package.c)
  endif()
  if(WIN32)
    # Milestone 5: repack the 7z-SFX Windows updater .exe -- shells out to
    # a 7z-compatible CLI (see app/win_repack.c's own TODO on bundling
    # 7za.exe so end users install nothing). NOT YET buildable here (no
    # Windows host in this repo) -- see app/main.c's own top comment.
    list(APPEND GUI_APP_SOURCES app/win_repack.c)
  endif()
  add_executable(headrush-nam-gui ${GUI_APP_SOURCES})
  target_include_directories(headrush-nam-gui PRIVATE ${SDL2_INCLUDE_DIRS} third_party/nuklear)
  target_link_libraries(headrush-nam-gui PRIVATE nam_gui_core ${SDL2_LIBRARIES})
  add_dependencies(headrush-nam-gui nam_gui_blobs)
  # Nuklear (NK_IMPLEMENTATION, in app/main.c) calls libm functions
  # (acos/sin/sqrt/...) directly -- on macOS those are folded into
  # libSystem so this is a no-op there, but a separate libm.so is required
  # on Linux or linking fails with "undefined reference to acos@@GLIBC_*".
  find_library(MATH_LIBRARY m)
  if(MATH_LIBRARY)
    target_link_libraries(headrush-nam-gui PRIVATE ${MATH_LIBRARY})
  endif()
  if(APPLE)
    target_link_libraries(headrush-nam-gui PRIVATE "-framework Cocoa")
  endif()
else()
  message(WARNING "SDL2 not found -- skipping headrush-nam-gui target (gui-core-cli still builds). "
                   "macOS: brew install sdl2; Debian/Ubuntu: apt install libsdl2-dev.")
endif()
