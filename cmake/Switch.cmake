# Nintendo Switch build: SafeHouse.nro, one homebrew program (sdmc:/switch/SafeHouse/) holding the launcher, Sly 1 and
# Sly 2. Sly 3 can be installed but is not ported yet. Included by CMakeLists.txt with the devkitA64 toolchain
# (cmake/devkitA64-libnx.cmake):
#
#   cmake --preset switch-release
#   cmake --build --preset switch-release
#
# Sly 1 and Sly 2 share thousands of global names, so each game is first linked on its own into one relocatable object
# whose only global symbol is its descriptor (cane_game_<key>, platform/switch/runtime.h). The launcher and the Switch
# layer (cane_switch) are then linked with both objects.
#
# Requires devkitPro's switch-dev and switch-ffmpeg packages (dkp-pacman -S switch-dev switch-ffmpeg) and Python 3.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)   # newlib hides some POSIX declarations in strict ISO mode
set(CMAKE_C_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(PORTLIBS "${DEVKITPRO}/portlibs/switch")
# Release CI passes SAFEHOUSE_VERSION (x.y.z) as for the desktop packages. Without it the build is a development build
# that never checks for updates (platform/switch/launcher/update.h).
set(CANE_SWITCH_AUTHOR "naga")
set(SAFEHOUSE_VERSION "" CACHE STRING "Release version of the packages (x.y.z); empty for development builds")
if(SAFEHOUSE_VERSION AND NOT SAFEHOUSE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "SAFEHOUSE_VERSION must be x.y.z (got ${SAFEHOUSE_VERSION})")
endif()
set(nacp_version "${SAFEHOUSE_VERSION}")
if(NOT nacp_version)
  set(nacp_version "dev")
endif()
option(CANE_SWITCH_LTO "Link-time optimization for the Switch build (slower link, faster game)" ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(CANE_UAM NAMES uam HINTS "${DEVKITPRO}/tools/bin" REQUIRED)
find_program(CANE_ELF2NRO NAMES elf2nro HINTS "${DEVKITPRO}/tools/bin" REQUIRED)
find_program(CANE_NACPTOOL NAMES nacptool HINTS "${DEVKITPRO}/tools/bin" REQUIRED)
set(ffmpeg_libraries avformat avcodec swresample swscale avutil dav1d)
foreach(lib ${ffmpeg_libraries} z)
  if(NOT EXISTS "${PORTLIBS}/lib/lib${lib}.a")
    message(FATAL_ERROR "Missing the Switch portlib lib${lib}.a: install switch-ffmpeg (dkp-pacman -S switch-ffmpeg)")
  endif()
endforeach()
# avformat's own dependencies, where the portlibs have them
foreach(lib bz2 zstd lzma expat elf)
  if(EXISTS "${PORTLIBS}/lib/lib${lib}.a")
    list(APPEND ffmpeg_libraries ${lib})
  endif()
endforeach()

set(gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${gen}" "${gen}/prompts")
set(root "${CMAKE_CURRENT_SOURCE_DIR}")
set(imgui_root "${root}/Libraries/include/imgui")

# -fno-strict-aliasing: the decompiled code's type punning. -ffp-contract=off: keep the PC builds' float results (aarch64
# GCC fuses multiply-adds by default).
set(cane_switch_options -fno-strict-aliasing -ffp-contract=off "$<$<COMPILE_LANGUAGE:CXX>:-Wno-invalid-offsetof>")
set(cane_switch_defines GLFW_INCLUDE_NONE _CRT_SECURE_NO_WARNINGS IMGUI_DEFINE_MATH_OPERATORS CANE_DEKO3D)

# ---- generated files
# the installer's background
add_custom_command(OUTPUT "${gen}/switch_assets.h"
  COMMAND "${Python3_EXECUTABLE}" "${root}/cmake/switch_embed.py" "${gen}/switch_assets.h"
          cane_installer_background "${root}/platform/switch/installer-background.png"
  DEPENDS "${root}/platform/switch/installer-background.png" "${root}/cmake/switch_embed.py" VERBATIM)
# the launcher's covers (launcher/assets)
add_custom_command(OUTPUT "${gen}/launcher_assets.h"
  COMMAND "${Python3_EXECUTABLE}" "${root}/cmake/switch_embed.py" "${gen}/launcher_assets.h"
          cane_cover_sly1 "${root}/launcher/assets/steam/sly1/grid.png"
          cane_cover_sly2 "${root}/launcher/assets/steam/sly2/grid.png"
          cane_cover_sly3 "${root}/launcher/assets/steam/sly3/grid.jpg"
  DEPENDS "${root}/launcher/assets/steam/sly1/grid.png" "${root}/launcher/assets/steam/sly2/grid.png"
          "${root}/launcher/assets/steam/sly3/grid.jpg" "${root}/cmake/switch_embed.py" VERBATIM)
# the shaders, compiled offline for deko3d by uam: the shared GL layer's (launcher, Sly 2) and Sly 1's backend's
file(GLOB deko_shader_sources CONFIGURE_DEPENDS "${root}/platform/switch/deko/shaders/*"
  "${root}/Sly2-Release/*.vert" "${root}/Sly2-Release/*.frag" "${root}/Sly2-Release/*.geom")
add_custom_command(OUTPUT "${gen}/deko_shaders.cpp"
  COMMAND "${Python3_EXECUTABLE}" "${root}/cmake/switch_shaders.py" --source "${root}" --output "${gen}" --uam "${CANE_UAM}"
  DEPENDS ${deko_shader_sources} "${root}/cmake/switch_shaders.py" VERBATIM)
file(GLOB sly1_shader_sources CONFIGURE_DEPENDS "${root}/platform/switch/sly1/deko/shaders/*"
  "${root}/Sly1-Release/*.vert" "${root}/Sly1-Release/*.frag")
add_custom_command(OUTPUT "${gen}/sly1/deko/deko_shaders.h"
  COMMAND "${Python3_EXECUTABLE}" "${root}/cmake/switch_shaders_sly1.py" --source "${root}" --output "${gen}/sly1/deko" --uam "${CANE_UAM}"
  DEPENDS ${sly1_shader_sources} "${root}/cmake/switch_shaders_sly1.py" VERBATIM)

# ---- the Switch layer, shared by the launcher and the games
add_library(cane_switch STATIC
  "${root}/platform/switch/runtime.cpp"
  "${root}/platform/switch/glfw_switch.cpp"
  "${root}/platform/switch/audio_switch.cpp"
  "${root}/platform/switch/installer.cpp"
  "${root}/platform/switch/switch_ui.cpp"
  "${root}/platform/switch/saltynx.cpp"
  "${root}/platform/switch/io_worker.cpp"
  "${root}/platform/switch/deko/gl_deko.cpp"
  "${root}/platform/switch/deko/imgui_impl_deko.cpp"
  "${imgui_root}/imgui.cpp" "${imgui_root}/imgui_draw.cpp" "${imgui_root}/imgui_tables.cpp" "${imgui_root}/imgui_widgets.cpp"
  "${imgui_root}/imgui_impl_glfw.cpp"
  "${root}/Sly1-Release/glad.c"
  "${gen}/switch_assets.h")
target_include_directories(cane_switch BEFORE PUBLIC "${root}/platform/switch/include")
target_include_directories(cane_switch PUBLIC "${root}/Libraries/include" "${root}/platform/switch" "${gen}" "${PORTLIBS}/include")
target_compile_definitions(cane_switch PUBLIC ${cane_switch_defines})
target_compile_options(cane_switch PRIVATE ${cane_switch_options})
set_source_files_properties("${imgui_root}/imgui.cpp" "${imgui_root}/imgui_draw.cpp" "${imgui_root}/imgui_tables.cpp"
  "${imgui_root}/imgui_widgets.cpp" "${imgui_root}/imgui_impl_glfw.cpp" "${root}/Sly1-Release/glad.c" PROPERTIES COMPILE_OPTIONS "-w")
if(CANE_SWITCH_LTO)
  set_target_properties(cane_switch PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
endif()
# CHD disc images for the installers (platform/disc_image.h)
include("${root}/cmake/chd.cmake")
target_link_libraries(cane_switch PUBLIC cane_chd)

# libnx's switch.specs finds its linker script through the DEVKITPRO environment variable, and GCC's parallel LTO runs
# make: both are given to the link steps.
set(link_launcher "${CMAKE_COMMAND};-E;env;DEVKITPRO=${DEVKITPRO}")
find_program(CANE_MAKE NAMES make gmake HINTS "${DEVKITPRO}/../../usr/bin")
if(CANE_MAKE)
  list(APPEND link_launcher "MAKE=${CANE_MAKE}")
endif()

# ---- the games: each one a static library of its sources, then one partially linked object (see the top). COMDAT
# groups are placed like plain sections so no section is shared with the other game or the launcher in the final link.
# -flto-partition=none: GCC's LTO partitioner fails an assertion on incremental links (lto-partition.cc
# add_symbol_to_partition_1), so each game's LTO runs as one job.
string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type)
separate_arguments(game_link_flags UNIX_COMMAND "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${build_type}}")
list(APPEND game_link_flags -r -nostdlib -Wl,--force-group-allocation -Wl,-d)
if(CANE_SWITCH_LTO)
  list(APPEND game_link_flags -flto=auto -flinker-output=nolto-rel -flto-partition=none)
endif()
set(cane_switch_game_objects "")
function(cane_switch_game target key)
  target_link_libraries(${target} PRIVATE cane_switch)
  target_compile_options(${target} PRIVATE ${cane_switch_options})
  if(CANE_SWITCH_LTO)
    set_target_properties(${target} PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
  endif()
  set(partial "${CMAKE_CURRENT_BINARY_DIR}/${key}.partial.o")
  set(object "${CMAKE_CURRENT_BINARY_DIR}/${key}.o")
  add_custom_command(OUTPUT "${object}"
    COMMAND ${link_launcher} "${CMAKE_CXX_COMPILER}" ${game_link_flags} -o "${partial}"
            -Wl,--whole-archive "$<TARGET_FILE:${target}>" -Wl,--no-whole-archive
    COMMAND "${CMAKE_OBJCOPY}" "--keep-global-symbol=cane_game_${key}" "${partial}" "${object}"
    DEPENDS ${target}
    COMMENT "Linking the game object ${key}.o (every symbol local but cane_game_${key})"
    VERBATIM)
  set(cane_switch_game_objects ${cane_switch_game_objects} "${object}" PARENT_SCOPE)
endfunction()

# ---- Sly 2
file(GLOB sly2_sources CONFIGURE_DEPENDS "${root}/Sly2-Release/*.cpp" "${root}/Sly2-Release/*.c")
add_library(sly2_game STATIC ${sly2_sources} "${root}/Libraries/miniz/miniz.c" "${root}/assets/prompts/prompt_data.cpp")
cane_switch_game(sly2_game sly2)
target_include_directories(sly2_game PRIVATE "${root}/Sly2-Release" "${root}/platform" "${root}/Libraries/miniz"
  "${root}/assets/prompts" "${gen}/prompts" "${gen}/sly2")
target_compile_definitions(sly2_game PRIVATE "SLY2N_DEV_ROOT=\"sdmc:/switch/SafeHouse\"" CANE_HAVE_PROMPT_ATLAS=1 CANE_HAVE_GAMECONTROLLERDB=1)
set_source_files_properties("${root}/Libraries/miniz/miniz.c" PROPERTIES COMPILE_OPTIONS "-w")
file(MAKE_DIRECTORY "${gen}/sly2")
# the game's shader sources (gl.cpp includes them; deko3d runs the offline programs)
add_custom_command(OUTPUT "${gen}/sly2/embedded_shaders.h"
  COMMAND ${CMAKE_COMMAND} -DSOURCE=${root}/Sly2-Release -DOUTPUT=${gen}/sly2/embedded_shaders.h -P ${root}/cmake/embed-shaders.cmake
  DEPENDS ${root}/cmake/embed-shaders.cmake VERBATIM)
# the front end's logo (qolview.cpp)
add_custom_command(OUTPUT "${gen}/sly2/sly2n_logo.h"
  COMMAND "${Python3_EXECUTABLE}" "${root}/cmake/switch_embed.py" "${gen}/sly2/sly2n_logo.h" sly2n_logo_png "${root}/launcher/assets/games/sly2-logo.png"
  DEPENDS "${root}/launcher/assets/games/sly2-logo.png" "${root}/cmake/switch_embed.py" VERBATIM)
# the controller prompt atlases and SDL_GameControllerDB (prompt_data.cpp)
set(prompt_headers "")
foreach(family keyboard playstation xbox nintendo steamdeck steamcontroller ps4 ps5 steamcontroller2026 devices)
  set(header "${gen}/prompts/prompts_${family}_png.h")
  add_custom_command(OUTPUT "${header}"
    COMMAND "${Python3_EXECUTABLE}" "${root}/cmake/switch_embed.py" "${header}" cane_prompts_${family} "${root}/assets/prompts/prompts_${family}.png"
    DEPENDS "${root}/assets/prompts/prompts_${family}.png" "${root}/cmake/switch_embed.py" VERBATIM)
  list(APPEND prompt_headers "${header}")
endforeach()
add_custom_command(OUTPUT "${gen}/prompts/gamecontrollerdb_txt.h"
  COMMAND "${Python3_EXECUTABLE}" "${root}/cmake/switch_embed.py" "${gen}/prompts/gamecontrollerdb_txt.h" cane_gamecontrollerdb
          "${root}/Libraries/gamecontrollerdb/gamecontrollerdb.txt"
  DEPENDS "${root}/Libraries/gamecontrollerdb/gamecontrollerdb.txt" "${root}/cmake/switch_embed.py" VERBATIM)
list(APPEND prompt_headers "${gen}/prompts/gamecontrollerdb_txt.h")
# the debug menu's version line (debugmenu.cpp)
file(STRINGS "${root}/CMakeLists.txt" sly2_project_line REGEX "^project\\(ProjectCane VERSION")
string(REGEX MATCH "VERSION ([0-9.]+)" sly2_version_match "${sly2_project_line}")
set(sly2_port_version "${CMAKE_MATCH_1}")
add_custom_target(sly2_version
  COMMAND ${CMAKE_COMMAND} -DROOT=${root} -DVERSION=${sly2_port_version} -DHASH=${CANE_GIT_HASH}
          -DOUTPUT=${gen}/sly2/sly2_version.h -P ${root}/cmake/sly2-version.cmake
  BYPRODUCTS "${gen}/sly2/sly2_version.h" VERBATIM)
add_dependencies(sly2_game sly2_version)
target_sources(sly2_game PRIVATE "${gen}/sly2/embedded_shaders.h" "${gen}/sly2/sly2n_logo.h" ${prompt_headers})

# ---- Sly 1: the vcxproj's sources, as the desktop build takes them
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${root}/Sly1-Release/Sly1-Release.vcxproj")
file(STRINGS "${root}/Sly1-Release/Sly1-Release.vcxproj" sly1_lines REGEX "<ClCompile Include=")
set(sly1_sources "")
foreach(line IN LISTS sly1_lines)
  string(REGEX REPLACE ".*Include=\"([^\"]+)\".*" "\\1" source "${line}")
  string(REPLACE "\\" "/" source "${source}")
  # no XAPO reverb: audio_switch.h runs the reverb itself
  if(NOT source MATCHES "^\\.\\." AND NOT source STREQUAL "glad.c" AND NOT source STREQUAL "spu_reverb_xapo.cpp")
    list(APPEND sly1_sources "${root}/Sly1-Release/${source}")
  endif()
endforeach()
# the installer's extraction: iso_extractor and the File Extractor's native tools
set(brx_rebuilder "${root}/File Extractor GUI/NativeTools/BrxRebuilder")
set(pal_extractor "${root}/File Extractor GUI/NativeTools/PalExtractor")
set(brx_sources "")
foreach(source AssetWalker BinaryInputStream BrxWalker GeometryWalker ObjectWalker OptionWalker SpliceWalker TriangleList)
  list(APPEND brx_sources "${brx_rebuilder}/${source}.cpp")
endforeach()
set(pal_sources "")
foreach(source AssetWalker BinaryInputStream BrxWalker GeometryWalker ObjectWalker OptionWalker PalIsoExtractor SpliceWalker)
  list(APPEND pal_sources "${pal_extractor}/${source}.cpp")
endforeach()
# Sly 1's deko3d backend and ImGui renderer, in Sly 1's object only
set(sly1_switch_sources "${root}/platform/installer/iso_extractor.cpp" "${root}/platform/switch/sly1/vtables.cpp"
  "${root}/platform/switch/sly1/audio_prefetch.cpp" "${root}/platform/switch/sly1/deko/backend.cpp"
  "${root}/platform/switch/sly1/deko/imgui.cpp")
file(MAKE_DIRECTORY "${gen}/sly1/deko")
add_library(sly1_game STATIC ${sly1_sources} "${imgui_root}/imgui_demo.cpp" "${imgui_root}/ImGuiFileDialog.cpp"
  "${root}/Libraries/miniz/miniz.c" "${root}/assets/prompts/prompt_data.cpp" ${sly1_switch_sources} ${brx_sources} ${pal_sources}
  "${gen}/sly1/deko/deko_shaders.h" ${prompt_headers})
cane_switch_game(sly1_game sly1)
target_include_directories(sly1_game BEFORE PRIVATE "${root}/platform/switch/sly1")
target_include_directories(sly1_game PRIVATE "${root}/Sly1-Release" "${root}/platform" "${root}/Libraries/miniz" "${gen}/sly1"
  "${gen}/sly1/deko" "${root}/assets/prompts" "${gen}/prompts")
# fused multiply-adds, as in Project Cane's Switch build (Sly 2 keeps -ffp-contract=off)
target_compile_options(sly1_game PRIVATE -ffp-contract=fast)
target_compile_definitions(sly1_game PRIVATE GLFW_STATIC GLM_ENABLE_EXPERIMENTAL USE_STD_FILESYSTEM IMGUI_IMPL_OPENGL_LOADER_CUSTOM
  _ENABLE_EXTENDED_ALIGNED_STORAGE CANE_HAVE_PROMPT_ATLAS=1 CANE_HAVE_GAMECONTROLLERDB=1)
# the native tools' CBinaryInputStream, renamed apart from the game's
set_source_files_properties(${brx_sources} "${root}/platform/installer/iso_extractor.cpp" PROPERTIES
  COMPILE_DEFINITIONS CBinaryInputStream=CaneBrxInputStream SKIP_PRECOMPILE_HEADERS ON)
set_source_files_properties(${pal_sources} ${sly1_switch_sources} "${imgui_root}/imgui_demo.cpp" "${imgui_root}/ImGuiFileDialog.cpp"
  "${root}/Libraries/miniz/miniz.c" "${root}/assets/prompts/prompt_data.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
set_source_files_properties("${imgui_root}/imgui_demo.cpp" "${imgui_root}/ImGuiFileDialog.cpp" PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties("${root}/Sly1-Release/alo.cpp" PROPERTIES COMPILE_DEFINITIONS ALO_IMPLEMENTATION SKIP_PRECOMPILE_HEADERS ON)
file(MAKE_DIRECTORY "${gen}/sly1")
# the desktop build's precompiled headers, compat.h first
set(sly1_pch "${root}/platform/switch/sly1/compat.h")
foreach(header platform/platform.h Sly1-Release/sw.h Sly1-Release/save.h Sly1-Release/freeze.h Sly1-Release/act.h Sly1-Release/wr.h
               Sly1-Release/shadow.h Sly1-Release/fader.h Sly1-Release/splicemap.h Sly1-Release/gc.h)
  list(APPEND sly1_pch "${root}/${header}")
endforeach()
target_precompile_headers(sly1_game PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${sly1_pch}>")

# ---- SafeHouse.nro: the launcher's main and the games' objects
set_source_files_properties(${cane_switch_game_objects} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_executable(safehouse "${root}/platform/switch/launcher/launcher.cpp" "${root}/platform/switch/launcher/update.cpp"
  "${root}/platform/switch/launcher/sly3.cpp" "${gen}/launcher_assets.h" "${gen}/deko_shaders.cpp" ${cane_switch_game_objects})
set_target_properties(safehouse PROPERTIES OUTPUT_NAME SafeHouse SUFFIX ".elf")
target_link_libraries(safehouse PRIVATE cane_switch)
target_compile_options(safehouse PRIVATE ${cane_switch_options})
target_compile_definitions(safehouse PRIVATE "CANE_SWITCH_VERSION=\"${SAFEHOUSE_VERSION}\"")
set_property(TARGET safehouse PROPERTY CXX_LINKER_LAUNCHER "${link_launcher}")
# every pthread starts on cores 1-2 and getenv is cached (platform/switch/runtime.cpp)
target_link_options(safehouse PRIVATE
  -Wl,--wrap=__syscall_thread_create -Wl,--wrap=getenv -Wl,--wrap=setenv -Wl,--wrap=unsetenv -Wl,--wrap=putenv)
target_link_libraries(safehouse PRIVATE
  -Wl,--start-group deko3d ${ffmpeg_libraries} z nx m stdc++ -Wl,--end-group)
if(CANE_SWITCH_LTO)
  set_target_properties(safehouse PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
  target_link_options(safehouse PRIVATE -flto=auto)
endif()
set(nacp "${CMAKE_CURRENT_BINARY_DIR}/SafeHouse.nacp")
set(nro "${CMAKE_CURRENT_BINARY_DIR}/nro/SafeHouse.nro")
# rewritten only when the author or version change
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/SafeHouse.nacp.txt" CONTENT "SafeHouse\n${CANE_SWITCH_AUTHOR}\n${nacp_version}\n")
add_custom_command(OUTPUT "${nacp}"
  COMMAND "${CANE_NACPTOOL}" --create "SafeHouse" "${CANE_SWITCH_AUTHOR}" "${nacp_version}" "${nacp}"
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/SafeHouse.nacp.txt" VERBATIM)
add_custom_command(OUTPUT "${nro}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/nro"
  COMMAND "${CANE_ELF2NRO}" "$<TARGET_FILE:safehouse>" "${nro}" "--nacp=${nacp}" "--icon=${root}/platform/switch/icons/safehouse.jpg"
  DEPENDS safehouse "${nacp}" "${root}/platform/switch/icons/safehouse.jpg" VERBATIM)
add_custom_target(nro ALL DEPENDS "${nro}")
