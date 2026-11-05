# Sly3-Release: Sly 3 as a native source port, forked from Sly2-Release. Built standalone or from the
# top-level CMake (-DCANE_BUILD_SLY3=ON).
cmake_minimum_required(VERSION 3.20)
project(Sly3Release CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(CANE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

file(GLOB sly3n_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/sly3/*.cpp")
add_executable(Sly3-Release ${sly3n_sources})
target_include_directories(Sly3-Release PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CANE_ROOT}/Libraries/include" "${CANE_ROOT}/platform"
                           "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(Sly3-Release PRIVATE _CRT_SECURE_NO_WARNINGS)
# Development data: the checkout's build/ folder (the reference's RAM maps, the test memory card), used only by the
# developer options (--loadmaps, the development card's fallback save). A git worktree uses its main checkout's, so
# every working copy reads the same data. SLY2N_DEV_ROOT overrides it.
if(NOT SLY2N_DEV_ROOT)
  set(SLY2N_DEV_ROOT "${CANE_ROOT}")
  find_package(Git QUIET)
  if(GIT_EXECUTABLE)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --path-format=absolute --git-common-dir
                    WORKING_DIRECTORY "${CANE_ROOT}" OUTPUT_VARIABLE sly2n_git_common RESULT_VARIABLE sly2n_git_result
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(sly2n_git_result EQUAL 0 AND IS_DIRECTORY "${sly2n_git_common}")
      get_filename_component(SLY2N_DEV_ROOT "${sly2n_git_common}" DIRECTORY)
    endif()
  endif()
endif()
target_compile_definitions(Sly3-Release PRIVATE "SLY2N_DEV_ROOT=\"${SLY2N_DEV_ROOT}\"")
if(NOT MSVC)
  target_compile_options(Sly3-Release PRIVATE -fno-strict-aliasing -Wno-invalid-offsetof -ffp-contract=off)
endif()

# Renderer: GLFW window + OpenGL 4.3+ (glad from Sly1-Release), shaders embedded like Sly1-Release's.
if(NOT TARGET glfw)
  include(FetchContent)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  if(NOT FETCHCONTENT_SOURCE_DIR_GLFW AND EXISTS "$ENV{HOME}/Documents/tools/projectcane-deps/glfw-3.4/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_GLFW "$ENV{HOME}/Documents/tools/projectcane-deps/glfw-3.4")
  endif()
  FetchContent_Declare(glfw URL https://codeload.github.com/glfw/glfw/tar.gz/refs/tags/3.4 URL_HASH SHA256=c038d34200234d071fae9345bc455e4a8f2f544ab60150765d7704e08f3dac01)
  FetchContent_MakeAvailable(glfw)
endif()
target_sources(Sly3-Release PRIVATE "${CANE_ROOT}/Sly1-Release/glad.c")
target_link_libraries(Sly3-Release PRIVATE glfw ${CMAKE_DL_LIBS})
target_compile_definitions(Sly3-Release PRIVATE GLFW_INCLUDE_NONE)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(GLOB sly3n_shaders CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.vert" "${CMAKE_CURRENT_SOURCE_DIR}/*.frag")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_shaders.h"
  COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR} -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_shaders.h -P ${CANE_ROOT}/cmake/embed-shaders.cmake
  DEPENDS ${sly3n_shaders} ${CANE_ROOT}/cmake/embed-shaders.cmake VERBATIM)
target_sources(Sly3-Release PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_shaders.h")
# QoL (qolview.cpp): the launcher's high-quality logo, embedded (in Sly 2 it replaced the front end's logo texture;
# TODO(sly3): find Sly 3's logo shader before enabling the swap). (sly3n_logo_png).
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/sly3n_logo.h"
  COMMAND ${CMAKE_COMMAND} -DSOURCE=${CANE_ROOT}/launcher/assets/games/sly3-logo.png -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated/sly3n_logo.h -DSYMBOL=sly3n_logo_png -P ${CANE_ROOT}/cmake/embed-image.cmake
  DEPENDS ${CANE_ROOT}/launcher/assets/games/sly3-logo.png ${CANE_ROOT}/cmake/embed-image.cmake VERBATIM)
target_sources(Sly3-Release PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/sly3n_logo.h")

# Sound and movies (sound.cpp, snd989.cpp, spiop.cpp, fmv.cpp): FAudio (XAudio2 on SDL2, as Sly1-Release on Linux),
# FFmpeg's MPEG-2 decoder for the PSS movies, SDL2 for the stand-alone movie window (--play-movie).
if(WIN32)
  if(NOT CANE_FFMPEG_ROOT)
    set(CANE_FFMPEG_ROOT "${CANE_ROOT}/Libraries/ffmpeg/ffmpeg-master-latest-win64-lgpl-shared")
  endif()
  target_include_directories(Sly3-Release PRIVATE "${CANE_FFMPEG_ROOT}/include")
  target_link_directories(Sly3-Release PRIVATE "${CANE_FFMPEG_ROOT}/lib")
  target_link_libraries(Sly3-Release PRIVATE avcodec avutil swscale)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SLY3N_FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
  target_link_libraries(Sly3-Release PRIVATE PkgConfig::SLY3N_FFMPEG)
endif()
if(NOT TARGET SDL2::SDL2)
  find_package(SDL2 2.24 CONFIG QUIET)
endif()
if(NOT TARGET FAudio-shared AND NOT TARGET FAudio-static)
  include(FetchContent)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
  # Offline: -DFETCHCONTENT_SOURCE_DIR_FAUDIO=<FAudio-25.02 sources> (the dev machine's copy is found by itself).
  if(NOT FETCHCONTENT_SOURCE_DIR_FAUDIO AND EXISTS "$ENV{HOME}/Documents/tools/projectcane-deps/FAudio-25.02/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_FAUDIO "$ENV{HOME}/Documents/tools/projectcane-deps/FAudio-25.02")
  endif()
  FetchContent_Declare(faudio URL https://codeload.github.com/FNA-XNA/FAudio/tar.gz/refs/tags/25.02
                       URL_HASH SHA256=eab667d231e5036b0e5c3c6f89fb4ea792284493d46161bc056a88a6dadc2683)
  FetchContent_MakeAvailable(faudio)
endif()
if(TARGET FAudio-shared)
  target_link_libraries(Sly3-Release PRIVATE FAudio-shared)
else()
  target_link_libraries(Sly3-Release PRIVATE FAudio-static)
endif()
if(TARGET SDL2::SDL2)
  target_link_libraries(Sly3-Release PRIVATE SDL2::SDL2)
else()
  target_include_directories(Sly3-Release PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(Sly3-Release PRIVATE ${SDL2_LIBRARIES})
endif()
find_package(Threads REQUIRED)
target_link_libraries(Sly3-Release PRIVATE Threads::Threads)

# Controller prompt atlases (Kenney Input Prompts 1.5A, CC0: licenses/kenney-input-prompts) and SDL_GameControllerDB,
# embedded as cmake/prompts.cmake does for the launcher (platform/button_prompts.h, gamepad_mappings.h): the dynamic
# button icons (promptglyph.cpp) and the bundled pad mappings.
set(sly3n_prompt_source "${CANE_ROOT}/assets/prompts")
set(sly3n_gamecontrollerdb "${CANE_ROOT}/Libraries/gamecontrollerdb/gamecontrollerdb.txt")
if(EXISTS "${sly3n_prompt_source}/prompt_atlas_rects.inc" AND EXISTS "${sly3n_gamecontrollerdb}")
  set(sly3n_prompt_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/prompts")
  file(MAKE_DIRECTORY "${sly3n_prompt_generated}")
  set(sly3n_prompt_headers "")
  foreach(family keyboard playstation xbox nintendo steamdeck steamcontroller ps4 ps5 steamcontroller2026 devices)
    set(image "${sly3n_prompt_source}/prompts_${family}.png")
    set(header "${sly3n_prompt_generated}/prompts_${family}_png.h")
    add_custom_command(OUTPUT "${header}" COMMAND ${CMAKE_COMMAND} -DSOURCE=${image} -DOUTPUT=${header} -DSYMBOL=cane_prompts_${family}
      -P ${CANE_ROOT}/cmake/embed-image.cmake DEPENDS "${image}" ${CANE_ROOT}/cmake/embed-image.cmake VERBATIM)
    list(APPEND sly3n_prompt_headers "${header}")
  endforeach()
  set(header "${sly3n_prompt_generated}/gamecontrollerdb_txt.h")
  add_custom_command(OUTPUT "${header}" COMMAND ${CMAKE_COMMAND} -DSOURCE=${sly3n_gamecontrollerdb} -DOUTPUT=${header} -DSYMBOL=cane_gamecontrollerdb
    -P ${CANE_ROOT}/cmake/embed-image.cmake DEPENDS "${sly3n_gamecontrollerdb}" ${CANE_ROOT}/cmake/embed-image.cmake VERBATIM)
  list(APPEND sly3n_prompt_headers "${header}")
  target_sources(Sly3-Release PRIVATE "${sly3n_prompt_source}/prompt_data.cpp" ${sly3n_prompt_headers})
  target_include_directories(Sly3-Release PRIVATE "${sly3n_prompt_source}" "${sly3n_prompt_generated}")
  target_compile_definitions(Sly3-Release PRIVATE CANE_HAVE_PROMPT_ATLAS=1 CANE_HAVE_GAMECONTROLLERDB=1)
endif()
