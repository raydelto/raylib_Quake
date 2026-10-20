cmake_minimum_required(VERSION 3.16)

# macOS: must be set before project(). Builds for the host architecture by
# default; -DCMAKE_OSX_ARCHITECTURES=arm64 or =x86_64 (Intel / Rosetta) picks
# one explicitly. Universal builds are not supported.
if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

project(raylib_Quake C)

if(APPLE)
  list(LENGTH CMAKE_OSX_ARCHITECTURES quake_osx_arch_count)
  if(quake_osx_arch_count GREATER 1)
    message(FATAL_ERROR "Universal builds are not supported; build once per "
      "architecture: -DCMAKE_OSX_ARCHITECTURES=arm64 or x86_64")
  endif()
endif()

# The WinQuake software renderer. With QUAKE_RAYLIB, `quake` runs with the
# raylib video, sound, input and system drivers; `quake-null` (always built)
# uses the null drivers, has no raylib in it, and is the dedicated server /
# headless build. Without QUAKE_RAYLIB, `quake` is the null build.
option(QUAKE_RAYLIB "Build with the raylib video/sound/input drivers" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  if(EMSCRIPTEN)
    # Debug info would make the .wasm several times larger.
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  else()
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
  endif()
endif()

# The browser build (emcmake cmake -B build-web, see web/web.cmake) makes
# `quake` and `quake-gl`, with raylib's web platform; it needs QUAKE_RAYLIB.
if(EMSCRIPTEN AND NOT QUAKE_RAYLIB)
  message(FATAL_ERROR "The browser build needs QUAKE_RAYLIB=ON")
endif()

# gnu11 keeps `false`/`true` usable as qboolean enumerators: they are
# keywords in C23, which GCC 15 uses by default.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(WQ ${CMAKE_CURRENT_SOURCE_DIR}/WinQuake)

set(QUAKE_CORE_SOURCES
  cl_demo.c cl_input.c cl_main.c cl_parse.c cl_tent.c chase.c
  cmd.c common.c console.c crc.c cvar.c
  host.c host_cmd.c keys.c menu.c mathlib.c model.c
  net_main.c net_loop.c net_dgrm.c net_vcr.c
  pr_cmds.c pr_edict.c pr_exec.c
  sbar.c screen.c view.c wad.c world.c zone.c
  sv_main.c sv_move.c sv_phys.c sv_user.c
  # software renderer (C versions of the x86 asm, id386 == 0)
  draw.c nonintel.c
  d_edge.c d_fill.c d_init.c d_modech.c d_part.c d_polyse.c d_scan.c
  d_sky.c d_sprite.c d_surf.c d_vars.c d_zpoint.c
  r_aclip.c r_alias.c r_bsp.c r_draw.c r_edge.c r_efrag.c r_light.c
  r_main.c r_misc.c r_part.c r_sky.c r_sprite.c r_surf.c r_vars.c
)

# Platform drivers
set(QUAKE_NULL_DRIVER_SOURCES
  sys_linux.c
  net_udp.c net_bsd.c
  vid_null.c snd_null.c in_null.c cd_null.c
)

# Video, input and system drivers use the bridge to quarantine raylib.h.
# Audio drivers implement Quake's DMA interface (snd_raylib.c) and CD audio (cd_raylib.c).
set(QUAKE_RAYLIB_DRIVER_SOURCES
  sys_raylib.c vid_raylib.c in_raylib.c bridge_raylib.c
  snd_dma.c snd_mem.c snd_mix.c snd_raylib.c cd_raylib.c
)
# A browser has no UDP sockets: id's net_none.c has the loopback driver
# only (single player, demos).
if(EMSCRIPTEN)
  list(APPEND QUAKE_RAYLIB_DRIVER_SOURCES net_none.c)
else()
  list(APPEND QUAKE_RAYLIB_DRIVER_SOURCES net_udp.c net_bsd.c)
endif()

# GLQuake (id's OpenGL renderer) for `quake-gl`: the same engine without
# the software renderer, compiled with GLQUAKE, drawing through
# gl_raylib.c, which runs GLQuake's OpenGL 1.1 calls on raylib's rlgl.
set(QUAKE_GL_CORE_SOURCES
  cl_demo.c cl_input.c cl_main.c cl_parse.c cl_tent.c chase.c
  cmd.c common.c console.c crc.c cvar.c
  host.c host_cmd.c keys.c menu.c mathlib.c
  net_main.c net_loop.c net_dgrm.c net_vcr.c
  pr_cmds.c pr_edict.c pr_exec.c
  sbar.c view.c wad.c world.c zone.c
  sv_main.c sv_move.c sv_phys.c sv_user.c
  r_part.c
  gl_draw.c gl_mesh.c gl_model.c gl_refrag.c gl_rlight.c gl_rmain.c
  gl_rmisc.c gl_rsurf.c gl_screen.c gl_test.c gl_warp.c
)
set(QUAKE_GL_DRIVER_SOURCES vid_glraylib.c gl_raylib.c)

list(TRANSFORM QUAKE_CORE_SOURCES PREPEND ${WQ}/)
list(TRANSFORM QUAKE_GL_CORE_SOURCES PREPEND ${WQ}/)
list(TRANSFORM QUAKE_GL_DRIVER_SOURCES PREPEND ${WQ}/)
list(TRANSFORM QUAKE_NULL_DRIVER_SOURCES PREPEND ${WQ}/)
list(TRANSFORM QUAKE_RAYLIB_DRIVER_SOURCES PREPEND ${WQ}/)

function(quake_compile_settings target)
  target_include_directories(${target} PRIVATE ${WQ})

  # Never use the x86 assembly, even on 32-bit x86.
  target_compile_definitions(${target} PRIVATE id386=0)

  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # The code relies on signed overflow wrapping and on type punning, and
    # assumes signed plain char (arm has unsigned char). -fno-common makes
    # Quake's tentative globals definitions explicit on old compilers (it is
    # already the default on GCC 10+ and Xcode 12+).
    target_compile_options(${target} PRIVATE
      -Wall
      -fwrapv -fno-strict-aliasing -fsigned-char -fno-common
      # Errors by default from GCC 14 on; keep older compilers just as strict.
      -Werror=implicit-function-declaration
      -Werror=implicit-int
      -Werror=int-conversion
      -Werror=incompatible-pointer-types
      -Werror=return-type
    )
  endif()
endfunction()

# The engine core is compiled once and linked into both executables.
add_library(quake_core OBJECT ${QUAKE_CORE_SOURCES})
quake_compile_settings(quake_core)

if(QUAKE_RAYLIB)
  # Use an installed raylib 6 if there is one, otherwise download raylib 6.0
  # and link it statically. -DCMAKE_DISABLE_FIND_PACKAGE_raylib=ON forces the
  # download; FETCHCONTENT_SOURCE_DIR_RAYLIB=DIR builds the source in DIR.
  if(NOT FETCHCONTENT_SOURCE_DIR_RAYLIB)
    find_package(raylib 6.0 QUIET)
  endif()
  if(raylib_FOUND)
    message(STATUS "Using installed raylib ${raylib_VERSION}")
  else()
    include(FetchContent)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
    # The default desktop backend (GLFW), not the experimental native ones;
    # the browser build uses raylib's web platform.
    if(EMSCRIPTEN)
      set(PLATFORM Web CACHE STRING "raylib platform" FORCE)
    else()
      set(PLATFORM Desktop CACHE STRING "raylib platform")
    endif()
    FetchContent_Declare(
      raylib
      GIT_REPOSITORY https://github.com/raysan5/raylib.git
      GIT_TAG 6.0
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(raylib)
    # raylib saves a PNG on F12 by itself; in Quake, F12 is bindable.
    target_compile_definitions(raylib PRIVATE SUPPORT_SCREEN_CAPTURE=0)
    message(STATUS "Using raylib 6.0 from ${raylib_SOURCE_DIR}")
  endif()

  add_executable(quake $<TARGET_OBJECTS:quake_core> ${QUAKE_RAYLIB_DRIVER_SOURCES})
  quake_compile_settings(quake)
  target_link_libraries(quake PRIVATE raylib m)

  # GLQuake: the raylib drivers with vid_glraylib.c in place of
  # vid_raylib.c. gl_raylib.c includes raylib's rlgl.h (not raylib.h).
  # In the browser build it is quake-gl.js (WebGL 2), see web/web.cmake.
  option(QUAKE_GL "Build quake-gl, GLQuake on raylib's rlgl" ON)
  if(QUAKE_GL)
    set(QUAKE_GL_SOURCES ${QUAKE_GL_CORE_SOURCES} ${QUAKE_RAYLIB_DRIVER_SOURCES}
      ${QUAKE_GL_DRIVER_SOURCES})
    list(REMOVE_ITEM QUAKE_GL_SOURCES ${WQ}/vid_raylib.c)
    add_executable(quake-gl ${QUAKE_GL_SOURCES})
    quake_compile_settings(quake-gl)
    target_compile_definitions(quake-gl PRIVATE GLQUAKE GLQUAKE_RAYLIB)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      # id's GL files are full of leftover locals and MSVC pragmas;
      # everything else -Wall reports was fixed.
      target_compile_options(quake-gl PRIVATE
        -Wno-unused-variable -Wno-unused-but-set-variable
        -Wno-unknown-pragmas)
    endif()
    if(NOT EMSCRIPTEN)
      target_link_libraries(quake-gl PRIVATE raylib m)
    endif()
  endif()

  if(EMSCRIPTEN)
    include(web/web.cmake)
    return()
  endif()

  set(QUAKE_NULL_TARGET quake-null)
else()
  set(QUAKE_NULL_TARGET quake)
endif()

add_executable(${QUAKE_NULL_TARGET} $<TARGET_OBJECTS:quake_core> ${QUAKE_NULL_DRIVER_SOURCES})
quake_compile_settings(${QUAKE_NULL_TARGET})
target_link_libraries(${QUAKE_NULL_TARGET} PRIVATE m)
