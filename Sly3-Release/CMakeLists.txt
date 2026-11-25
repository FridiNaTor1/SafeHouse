# Sly3-Release: Sly 3 as a native source port, a fresh fork of Sly2-Release (2026-11-05). Every Sly 3 number lives in game/ (game/sly3.h). Built standalone or from the top-level CMake (CANE_BUILD_SLY3).
cmake_minimum_required(VERSION 3.20)
project(Sly3Release CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(CANE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

file(GLOB sly3n_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/game/*.cpp")
# (platform/render: the renderer pieces Sly 2 and Sly 3 share, built against each game's own headers: FSR 1, texture mods)
list(APPEND sly3n_sources "${CANE_ROOT}/platform/render/fsr.cpp" "${CANE_ROOT}/platform/render/texmod.cpp")
# (test build: the files' static constructors in reverse order, as MinGW runs them on Windows: its CRT walks the .ctors
# list backwards. A Linux run of it shows a Windows-only static initialization order dependency; PACKAGING.md section 7)
option(SLY3_REVERSE_INIT_ORDER "Link Sly3-Release's sources in reverse order (init-order test build)" OFF)
if(SLY3_REVERSE_INIT_ORDER)
  list(SORT sly3n_sources)
  list(REVERSE sly3n_sources)
endif()
if(ANDROID)
  # (cmake/android.cmake) a JNI library, as Sly 2's: the game activity calls SDL_main (platform/android/native_entry.cpp),
  # which runs main.cpp's main under another name; the tool service runs the same entry for --convert-worlds and the
  # other tools. The activity loads libSly3-Release.so for the game "Sly3-Release" (GameActivity.java).
  add_library(Sly3-Release SHARED ${sly3n_sources} "${CANE_ROOT}/platform/android/native_entry.cpp")
  set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/main.cpp" PROPERTIES COMPILE_DEFINITIONS main=cane_sly3_main)
  target_compile_definitions(Sly3-Release PRIVATE CANE_ANDROID_GAME_ENTRY CANE_ANDROID_GAME_MAIN=cane_sly3_main)
  target_compile_options(Sly3-Release PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${CANE_ROOT}/platform/android/sly_android_compat.h>"
                         -Wno-error=format-security)
elseif(CANE_IOS)
  # (cmake/ios.cmake) Sly3Release.framework, as Sly 2's: the app's bridge runs main.cpp's main under another name in the
  # app's own process (platform/ios/program_entry.cpp, ios_app.mm), the game on the main thread, the tools on a worker
  add_library(Sly3-Release SHARED ${sly3n_sources} "${CANE_ROOT}/platform/ios/program_entry.cpp")
  set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/main.cpp" PROPERTIES COMPILE_DEFINITIONS main=cane_sly3_main)
  target_compile_definitions(Sly3-Release PRIVATE CANE_IOS_ENTRY=cane_sly3_main "CANE_IOS_ROLE=\"game\"")
  target_link_libraries(Sly3-Release PRIVATE cane_platform)
else()
  add_executable(Sly3-Release ${sly3n_sources})
endif()
target_include_directories(Sly3-Release PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CANE_ROOT}/Libraries/include" "${CANE_ROOT}/platform"
                           "${CANE_ROOT}/platform/render"
                           "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(Sly3-Release PRIVATE _CRT_SECURE_NO_WARNINGS "CANE_TEXMOD_CACHE_GAME=\"sly3\"")
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
# The debug menu's title line (debugmenu.cpp): the release version (SAFEHOUSE_VERSION, the one the launcher shows; CI
# passes it) and the checkout's short git hash, regenerated at every build (cmake/sly2-version.cmake).
file(STRINGS "${CANE_ROOT}/CMakeLists.txt" sly2n_project_line REGEX "^project\\(ProjectCane VERSION")
string(REGEX MATCH "VERSION ([0-9.]+)" sly2n_version_match "${sly2n_project_line}")
set(SLY2N_PORT_VERSION "${CMAKE_MATCH_1}")
if(NOT SLY2N_PORT_VERSION)
  set(SLY2N_PORT_VERSION "0.0.0")
endif()
# (a hash from outside the checkout, e.g. the packaging container's: -DCANE_GIT_HASH=<hash>, or the environment
# variable CANE_GIT_HASH at build time; empty: git)
set(CANE_GIT_HASH "" CACHE STRING "The git hash for the Sly 2 port's version string (empty: ask git)")
add_custom_target(sly3_version
  COMMAND ${CMAKE_COMMAND} -DROOT=${CANE_ROOT} -DVERSION=${SLY2N_PORT_VERSION} -DRELEASE=${SAFEHOUSE_VERSION} -DHASH=${CANE_GIT_HASH}
          -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated/sly2_version.h -P ${CANE_ROOT}/cmake/sly2-version.cmake
  BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/generated/sly2_version.h" VERBATIM)
add_dependencies(Sly3-Release sly3_version)
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
target_sources(Sly3-Release PRIVATE "${CANE_ROOT}/Libraries/glad/glad.c")
# miniz (MIT; Libraries/miniz): inflate for the PS3 HD import's .zip sources (platform/ps3disc.h, ps3hd.cpp)
target_sources(Sly3-Release PRIVATE "${CANE_ROOT}/Libraries/miniz/miniz.c")
target_include_directories(Sly3-Release PRIVATE "${CANE_ROOT}/Libraries/miniz")
# LZ4 1.10.0 (BSD 2-Clause; Libraries/lz4): the texture cache's records (platform/texture_cache.h, texmod.cpp)
target_sources(Sly3-Release PRIVATE "${CANE_ROOT}/Libraries/lz4/lz4.c" "${CANE_ROOT}/Libraries/lz4/lz4hc.c")
target_include_directories(Sly3-Release PRIVATE "${CANE_ROOT}/Libraries/lz4")
target_link_libraries(Sly3-Release PRIVATE glfw ${CMAKE_DL_LIBS})
target_compile_definitions(Sly3-Release PRIVATE GLFW_INCLUDE_NONE)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(GLOB sly2n_shaders CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.vert" "${CMAKE_CURRENT_SOURCE_DIR}/*.frag" "${CMAKE_CURRENT_SOURCE_DIR}/*.geom")
# (and Sly 2's programs the shared renderer pieces draw with: platform/render/fsr.cpp's FSR 1 passes)
set(sly3n_shared_shaders "${CANE_ROOT}/Sly2-Release/vrfull.vert" "${CANE_ROOT}/Sly2-Release/fsreasu.frag" "${CANE_ROOT}/Sly2-Release/fsrrcas.frag")
string(REPLACE ";" "|" sly3n_shared_shaders_arg "${sly3n_shared_shaders}")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_shaders.h"
  COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR} "-DEXTRA=${sly3n_shared_shaders_arg}" -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_shaders.h -P ${CANE_ROOT}/cmake/embed-shaders.cmake
  DEPENDS ${sly2n_shaders} ${sly3n_shared_shaders} ${CANE_ROOT}/cmake/embed-shaders.cmake VERBATIM)
target_sources(Sly3-Release PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_shaders.h")
# Vulkan renderer (platform/vulkan: the game's GL calls on Vulkan, chosen at run time, OpenGL the fallback; notes/
# VULKAN_2026-11-10.md): Linux, Windows and Android (as Sly 2's: OpenGL / GLES the fallback). The programs are compiled to SPIR-V at build time (cmake/vulkan_shaders.py,
# glslangValidator); without glslangValidator or Python the game builds without it (OpenGL only).
if(WIN32 OR ANDROID OR (UNIX AND NOT APPLE))
  set(sly3n_vulkan_default ON)
else()
  set(sly3n_vulkan_default OFF)
endif()
option(SLY3_VULKAN "Sly 3's Vulkan renderer (needs glslangValidator and Python 3 at build time)" ${sly3n_vulkan_default})
if(SLY3_VULKAN)
  find_program(SLY3N_GLSLANG NAMES glslangValidator glslang)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(SLY3N_GLSLANG AND Python3_Interpreter_FOUND)
    file(GLOB sly3n_vk_native_shaders CONFIGURE_DEPENDS "${CANE_ROOT}/platform/vulkan/shaders/*")
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/vk_shaders.cpp"
      COMMAND ${Python3_EXECUTABLE} ${CANE_ROOT}/cmake/vulkan_shaders.py --source ${CANE_ROOT} --output ${CMAKE_CURRENT_BINARY_DIR}/generated
              --glslang ${SLY3N_GLSLANG} --game Sly3-Release
      DEPENDS ${sly2n_shaders} ${sly3n_vk_native_shaders} ${CANE_ROOT}/cmake/vulkan_shaders.py ${CANE_ROOT}/cmake/switch_shaders.py VERBATIM)
    target_sources(Sly3-Release PRIVATE "${CANE_ROOT}/platform/vulkan/gl_vulkan.cpp" "${CANE_ROOT}/platform/vulkan/vk_loader.cpp"
                   "${CMAKE_CURRENT_BINARY_DIR}/generated/vk_shaders.cpp")
    target_include_directories(Sly3-Release PRIVATE "${CANE_ROOT}/platform/vulkan")
    target_compile_definitions(Sly3-Release PRIVATE CANE_VULKAN=1)
    message(STATUS "Sly 3: Vulkan renderer on (${SLY3N_GLSLANG})")
  else()
    message(WARNING "Sly 3: no glslangValidator or Python 3: built without the Vulkan renderer (OpenGL only)")
  endif()
endif()

# QoL (qolview.cpp): the launcher's high-quality Sly 2 logo, embedded (it replaces the front end's logo texture)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/sly2n_logo.h"
  COMMAND ${CMAKE_COMMAND} -DSOURCE=${CANE_ROOT}/launcher/assets/games/sly3-logo.png -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated/sly2n_logo.h -DSYMBOL=sly2n_logo_png -P ${CANE_ROOT}/cmake/embed-image.cmake
  DEPENDS ${CANE_ROOT}/launcher/assets/games/sly3-logo.png ${CANE_ROOT}/cmake/embed-image.cmake VERBATIM)
target_sources(Sly3-Release PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/sly2n_logo.h")

# Sound and movies (sound.cpp, snd989.cpp, spiop.cpp, fmv.cpp): FAudio (XAudio2 on SDL2, as Sly1-Release on Linux),
# FFmpeg's MPEG-2 decoder for the PSS movies, SDL2 for the stand-alone movie window (--play-movie).
if(ANDROID)
  target_link_libraries(Sly3-Release PRIVATE cane_android_ffmpeg)
elseif(WIN32)
  if(NOT CANE_FFMPEG_ROOT)
    set(CANE_FFMPEG_ROOT "${CANE_ROOT}/Libraries/ffmpeg/ffmpeg-master-latest-win64-lgpl-shared")
  endif()
  target_include_directories(Sly3-Release PRIVATE "${CANE_FFMPEG_ROOT}/include")
  target_link_directories(Sly3-Release PRIVATE "${CANE_FFMPEG_ROOT}/lib")
  target_link_libraries(Sly3-Release PRIVATE avcodec avutil swscale winmm)   # (winmm: platform/frame_pacer.h's timeBeginPeriod)
else()
  include("${CANE_ROOT}/cmake/ffmpeg.cmake")
  cane_link_ffmpeg(Sly3-Release avcodec avutil swscale)
endif()
if(APPLE)
  target_link_libraries(Sly3-Release PRIVATE cane_gl_metal)
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
if(CANE_IOS)
  # (cmake/ios.cmake: SDL is linked whole into SafeHouseCore, which every program shares)
  target_link_libraries(Sly3-Release PRIVATE cane_sdl)
elseif(TARGET SDL2::SDL2)
  target_link_libraries(Sly3-Release PRIVATE SDL2::SDL2)
else()
  target_include_directories(Sly3-Release PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(Sly3-Release PRIVATE ${SDL2_LIBRARIES})
endif()
# Controller rumble (rumble.cpp, platform/pad_rumble.h): SDL drives the motors only; input stays on GLFW (as Sly1-Release).
target_compile_definitions(Sly3-Release PRIVATE CANE_SDL_RUMBLE)
find_package(Threads REQUIRED)
target_link_libraries(Sly3-Release PRIVATE Threads::Threads)

# Controller prompt atlases (Kenney Input Prompts 1.5A, CC0: licenses/kenney-input-prompts) and SDL_GameControllerDB,
# embedded as cmake/prompts.cmake does for the launcher (platform/button_prompts.h, gamepad_mappings.h): the dynamic
# button icons (promptglyph.cpp) and the bundled pad mappings.
set(sly2n_prompt_source "${CANE_ROOT}/assets/prompts")
set(sly2n_gamecontrollerdb "${CANE_ROOT}/Libraries/gamecontrollerdb/gamecontrollerdb.txt")
if(EXISTS "${sly2n_prompt_source}/prompt_atlas_rects.inc" AND EXISTS "${sly2n_gamecontrollerdb}")
  set(sly2n_prompt_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/prompts")
  file(MAKE_DIRECTORY "${sly2n_prompt_generated}")
  set(sly2n_prompt_headers "")
  foreach(family keyboard playstation xbox nintendo steamdeck steamcontroller ps4 ps5 steamcontroller2026 gamecube switch2 wii wiiu metaquest valveindex steamframe vivewand pico devices)
    set(image "${sly2n_prompt_source}/prompts_${family}.png")
    set(header "${sly2n_prompt_generated}/prompts_${family}_png.h")
    add_custom_command(OUTPUT "${header}" COMMAND ${CMAKE_COMMAND} -DSOURCE=${image} -DOUTPUT=${header} -DSYMBOL=cane_prompts_${family}
      -P ${CANE_ROOT}/cmake/embed-image.cmake DEPENDS "${image}" ${CANE_ROOT}/cmake/embed-image.cmake VERBATIM)
    list(APPEND sly2n_prompt_headers "${header}")
  endforeach()
  set(header "${sly2n_prompt_generated}/gamecontrollerdb_txt.h")
  add_custom_command(OUTPUT "${header}" COMMAND ${CMAKE_COMMAND} -DSOURCE=${sly2n_gamecontrollerdb} -DOUTPUT=${header} -DSYMBOL=cane_gamecontrollerdb
    -P ${CANE_ROOT}/cmake/embed-image.cmake DEPENDS "${sly2n_gamecontrollerdb}" ${CANE_ROOT}/cmake/embed-image.cmake VERBATIM)
  list(APPEND sly2n_prompt_headers "${header}")
  target_sources(Sly3-Release PRIVATE "${sly2n_prompt_source}/prompt_data.cpp" ${sly2n_prompt_headers})
  target_include_directories(Sly3-Release PRIVATE "${sly2n_prompt_source}" "${sly2n_prompt_generated}")
  target_compile_definitions(Sly3-Release PRIVATE CANE_HAVE_PROMPT_ATLAS=1 CANE_HAVE_GAMECONTROLLERDB=1)
endif()

# The map editor's 3D view for Sly 3 (editor.cpp: the shell shared with Sly 2, editor/game/; --editor W; Sly2-Release/
# notes/EDITOR_2026-11-12.md "Phase 5"): desktop Linux / Windows only, as Sly 2's. ImGui on OpenGL. Every other build
# compiles the stubs. The mod-package loading (platform/mod_package.h, disc.cpp) is in every build.
if((WIN32 OR (UNIX AND NOT APPLE)) AND NOT ANDROID AND NOT CANE_IOS)
  set(sly3n_editor_default ON)
else()
  set(sly3n_editor_default OFF)
endif()
option(SLY3_EDITOR "Sly 3's map editor (desktop)" ${sly3n_editor_default})
if(SLY3_EDITOR)
  if(NOT TARGET cane_editor_core)
    add_subdirectory("${CANE_ROOT}/editor" "${CMAKE_CURRENT_BINARY_DIR}/editor")
  endif()
  set(sly3n_imgui "${CANE_ROOT}/Libraries/include/imgui")
  target_sources(Sly3-Release PRIVATE ${sly3n_imgui}/imgui.cpp ${sly3n_imgui}/imgui_draw.cpp ${sly3n_imgui}/imgui_tables.cpp
                 ${sly3n_imgui}/imgui_widgets.cpp ${sly3n_imgui}/imgui_impl_glfw.cpp ${sly3n_imgui}/imgui_impl_opengl3.cpp)
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/editor_font.h"
    COMMAND ${CMAKE_COMMAND} -DSOURCE=${CANE_ROOT}/launcher/assets/AtkinsonHyperlegible-Regular.ttf
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated/editor_font.h -P ${CANE_ROOT}/cmake/embed-font.cmake
    DEPENDS ${CANE_ROOT}/launcher/assets/AtkinsonHyperlegible-Regular.ttf ${CANE_ROOT}/cmake/embed-font.cmake VERBATIM)
  target_sources(Sly3-Release PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/editor_font.h")
  target_link_libraries(Sly3-Release PRIVATE cane_editor_core)
  target_compile_definitions(Sly3-Release PRIVATE SLY3_EDITOR=1)
endif()
