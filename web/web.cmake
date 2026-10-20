# Browser build:
#   ./tools/fetch_shareware.sh
#   emcmake cmake -S . -B build-web && cmake --build build-web
# Makes quake.html (the page, from web/shell.html; also copied to
# index.html), quake.js, quake.wasm and quake.data (the shareware pak0.pak,
# preloaded into /quake/id1), plus the shareware license texts, the GPL and
# an .htaccess next to them. See docs/WEB.md.

set(QUAKE_WEB_PAK ${CMAKE_SOURCE_DIR}/data/id1/pak0.pak CACHE FILEPATH
  "Shareware Quake 1.06 pak0.pak to preload (tools/fetch_shareware.sh)")
set(QUAKE_WEB_LICENSES ${CMAKE_SOURCE_DIR}/data/shareware CACHE PATH
  "Directory holding the shareware SLICNSE.TXT and LICINFO.TXT")

# Only the unmodified shareware pak0.pak may be bundled (SLICNSE.TXT);
# registered data never goes into a page.
set(QUAKE_SHAREWARE_PAK_SHA256
  35a9c55e5e5a284a159ad2a62e0e8def23d829561fe2f54eb402dbc0a9a946af)
if(NOT EXISTS ${QUAKE_WEB_PAK})
  message(FATAL_ERROR "${QUAKE_WEB_PAK} is missing: run tools/fetch_shareware.sh")
endif()
file(SHA256 ${QUAKE_WEB_PAK} quake_pak_hash)
if(NOT quake_pak_hash STREQUAL QUAKE_SHAREWARE_PAK_SHA256)
  message(FATAL_ERROR "${QUAKE_WEB_PAK} is not the shareware 1.06 pak0.pak")
endif()
foreach(f SLICNSE.TXT LICINFO.TXT)
  if(NOT EXISTS ${QUAKE_WEB_LICENSES}/${f})
    message(FATAL_ERROR "${QUAKE_WEB_LICENSES}/${f} is missing: run "
      "tools/fetch_shareware.sh (the license must ship with the shareware)")
  endif()
endforeach()

set(QUAKE_SHELL ${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html)

set_target_properties(quake PROPERTIES
  SUFFIX .html
  LINK_DEPENDS "${QUAKE_SHELL};${QUAKE_WEB_PAK}")

target_link_options(quake PRIVATE
  # The 16 MB hunk, the frame buffers and the surface cache fit in 64 MB.
  -sINITIAL_MEMORY=64MB
  -sALLOW_MEMORY_GROWTH=1
  # The edge renderer keeps its edge and surface lists on the stack.
  -sSTACK_SIZE=2MB
  -sENVIRONMENT=web
  # The page starts the game from a click, so the audio context may start
  # and the pointer can be captured.
  -sINVOKE_RUN=0
  -lidbfs.js
  --shell-file ${QUAKE_SHELL}
  --preload-file ${QUAKE_WEB_PAK}@/quake/id1/pak0.pak)

# The page calls callMain and uses FS; raylib's miniaudio reads
# Module.HEAPF32 from its Web Audio callback. raylib's own link options set
# -sEXPORTED_RUNTIME_METHODS=ccall, and the last one on the command line
# wins, so this goes with the libraries, which come after them.
target_link_libraries(quake PRIVATE
  -sEXPORTED_RUNTIME_METHODS=ccall,callMain,FS,addRunDependency,removeRunDependency,HEAPF32)

# The license texts go next to the page, which links to them. index.html is
# the page under the name a web server serves for the directory, and
# .htaccess sets the MIME types on Apache/LiteSpeed hosts.
add_custom_command(TARGET quake POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${QUAKE_WEB_LICENSES}/SLICNSE.TXT ${QUAKE_WEB_LICENSES}/LICINFO.TXT
    ${CMAKE_SOURCE_DIR}/gnu.txt
    $<TARGET_FILE_DIR:quake>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:quake> $<TARGET_FILE_DIR:quake>/index.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/web/htaccess $<TARGET_FILE_DIR:quake>/.htaccess)

# ---------------------------------------------------------------------------
# quake-gl: GLQuake on WebGL 2 (quake-gl.js, quake-gl.wasm). The page loads
# it instead of quake.js for ?renderer=gl. It needs raylib built for OpenGL
# ES 3 (WebGL 2) while `quake` keeps raylib's default ES 2 (WebGL 1), so
# raylib's sources are compiled a second time here as raylib_es3.
if(QUAKE_GL)
  if(NOT raylib_SOURCE_DIR)
    message(FATAL_ERROR "The browser quake-gl needs raylib's sources "
      "(the downloaded raylib, or FETCHCONTENT_SOURCE_DIR_RAYLIB)")
  endif()
  set(RAYLIB_ES3_SOURCES rcore.c rshapes.c rtextures.c rtext.c rmodels.c raudio.c)
  list(TRANSFORM RAYLIB_ES3_SOURCES PREPEND ${raylib_SOURCE_DIR}/src/)
  add_library(raylib_es3 STATIC ${RAYLIB_ES3_SOURCES})
  target_include_directories(raylib_es3 PUBLIC ${raylib_SOURCE_DIR}/src)
  target_compile_definitions(raylib_es3
    PUBLIC PLATFORM_WEB GRAPHICS_API_OPENGL_ES3
    PRIVATE SUPPORT_SCREEN_CAPTURE=0)
  target_compile_options(raylib_es3 PRIVATE -w)
  target_link_options(raylib_es3 PUBLIC -sUSE_GLFW=3
    -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
    # gl_raylib.c gets four GL entry points from rlGetProcAddress
    -sGL_ENABLE_GET_PROC_ADDRESS=1)

  set_target_properties(quake-gl PROPERTIES
    SUFFIX .js
    LINK_DEPENDS "${QUAKE_WEB_PAK}")
  # As for quake, but no shell (the page is quake.html) and a smaller stack:
  # GLQuake has no edge lists on it.
  target_link_options(quake-gl PRIVATE
    -sINITIAL_MEMORY=64MB
    -sALLOW_MEMORY_GROWTH=1
    -sSTACK_SIZE=1MB
    -sENVIRONMENT=web
    -sINVOKE_RUN=0
    -lidbfs.js
    --preload-file ${QUAKE_WEB_PAK}@/quake/id1/pak0.pak)
  target_link_libraries(quake-gl PRIVATE raylib_es3 m
    -sEXPORTED_RUNTIME_METHODS=ccall,callMain,FS,addRunDependency,removeRunDependency,HEAPF32)

  # quake-gl.data is the same package as quake.data (one file at the same
  # path); the page fetches quake.data for both, so a browser downloads and
  # caches the PAK once. Check that it really is the same.
  add_custom_command(TARGET quake-gl POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E compare_files
      $<TARGET_FILE_DIR:quake-gl>/quake-gl.data $<TARGET_FILE_DIR:quake>/quake.data)
  add_dependencies(quake-gl quake)
endif()
