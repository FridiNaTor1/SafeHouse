# The iOS / iPadOS build (Sly2-Release/notes/IOS_2026-11-11.md): the SafeHouse launcher, the extractor and Sly 2 as
# frameworks of one app, SafeHouse.app. Included from CMakeLists.txt right after project() when CMAKE_SYSTEM_NAME is
# iOS (cmake/ios-toolchain.cmake); the desktop targets below it are never configured for iOS. The app bundle and the
# .ipa are assembled by scripts/ios/package.py from what this builds.
#
#   SafeHouse              the executable (platform/ios/main.mm): SDL's UIKit app, then the launcher's framework
#   SafeHouseCore          SDL2 (whole) + the app bridge (platform/ios/ios_app.mm, touch_overlay.mm), shared by all
#   SafeHouseLauncher      the launcher (cane-launcher)
#   CaneExtractor          the disc extractor (cane-extractor)
#   Sly2Release            Sly 2 (Sly2-Release, on Metal: platform/metal); package.py copies it to Sly2Tools
#
# Like Android's JNI libraries, every program keeps its own image (iOS cannot start processes; ios_app.h).
set(CANE_IOS ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/frameworks")
if(NOT CMAKE_BUILD_TYPE)
 set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
string(TOLOWER "${CMAKE_BUILD_TYPE}" cane_ios_config)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
if(POLICY CMP0135)
 cmake_policy(SET CMP0135 NEW)
endif()
include(FetchContent)
find_package(Threads REQUIRED)
set(SAFEHOUSE_IOS_BUNDLE_ID "org.safehouse.ios" CACHE STRING "The app's bundle identifier (a signing profile's own id)")
message(STATUS "iOS: ${CMAKE_OSX_SYSROOT} ${CMAKE_OSX_ARCHITECTURES}, deployment target ${CMAKE_OSX_DEPLOYMENT_TARGET}")

# ---- SDL2 2.30.11 (as macOS / Windows), static, linked whole into SafeHouseCore so every program shares one SDL
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_STATIC_PIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
# (HIDAPI's iOS backend is Bluetooth LE for the Steam Controller and asks for the Bluetooth permission; the Xbox,
# PlayStation, MFi and Switch Pro controllers come through Apple's GameController framework, SDL's MFi driver)
set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
FetchContent_Declare(sdl2 URL https://codeload.github.com/libsdl-org/SDL/tar.gz/refs/tags/release-2.30.11
 URL_HASH SHA256=cc6136dd964854e8846c679703322f3e2a341d27a06a53f8b3f642c26f1b0cfd)
FetchContent_MakeAvailable(sdl2)
set(cane_sdl_includes "${sdl2_BINARY_DIR}/include" "${sdl2_BINARY_DIR}/include/SDL2"
 "${sdl2_BINARY_DIR}/include-config-${cane_ios_config}/SDL2")

# ---- The shared platform layer
add_library(cane_platform INTERFACE)
target_include_directories(cane_platform BEFORE INTERFACE
 "${CMAKE_CURRENT_SOURCE_DIR}/platform/ios/include"   # (ApplicationServices for glfw3native.h's Cocoa part)
 "${CMAKE_CURRENT_SOURCE_DIR}/platform/ios"
 "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/include"
 "${CMAKE_CURRENT_SOURCE_DIR}/platform")
target_compile_definitions(cane_platform INTERFACE CANE_IOS SDL_MAIN_HANDLED GLFW_INCLUDE_NONE IMGUI_IMPL_OPENGL_ES3
 IMGUI_DEFINE_MATH_OPERATORS GLES_SILENCE_DEPRECATION)
# The PS2 code expects x86's signed char and unfused multiply-adds (as the arm64 Linux / Android builds).
target_compile_options(cane_platform INTERFACE -fsigned-char -ffp-contract=off)
# (CoreFoundation: launcher/l10n.h's locale on Apple; the frameworks link what they use themselves)
target_link_libraries(cane_platform INTERFACE "-framework CoreFoundation" "-framework Foundation")

# SafeHouseCore.framework: SDL2 + the bridge
add_library(SafeHouseCore SHARED platform/ios/ios_app.mm platform/ios/touch_overlay.mm)
target_include_directories(SafeHouseCore PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/platform/ios" "${CMAKE_CURRENT_SOURCE_DIR}/platform")
target_compile_options(SafeHouseCore PRIVATE -fobjc-arc)
# (every SDL object, so the programs find all of SDL here: -force_load. CMake's WHOLE_ARCHIVE feature has no OBJCXX
# link rule on Apple, and this library links as Objective-C++)
target_link_options(SafeHouseCore PRIVATE "LINKER:-force_load,$<TARGET_FILE:SDL2-static>")
target_link_libraries(SafeHouseCore PRIVATE SDL2-static "-framework Foundation" "-framework UIKit"
 "-framework GameController" "-framework QuartzCore" "-framework UniformTypeIdentifiers" "-framework CoreGraphics")
# (the programs see SDL's headers and link SDL from SafeHouseCore: cane_sdl; Sly2-Release/CMakeLists.txt's CANE_IOS)
add_library(cane_sdl INTERFACE)
target_include_directories(cane_sdl INTERFACE ${cane_sdl_includes})
target_link_libraries(cane_sdl INTERFACE SafeHouseCore)
# (SDL2::SDL2 is SDL's own alias of SDL2-static: nothing here links it but SafeHouseCore)

# The frameworks: @rpath install names; the app's Frameworks folder holds them all (scripts/ios/package.py)
function(cane_ios_framework target name)
 string(TOLOWER "${name}" lower)
 set_target_properties(${target} PROPERTIES FRAMEWORK TRUE OUTPUT_NAME "${name}"
  MACOSX_FRAMEWORK_IDENTIFIER "${SAFEHOUSE_IOS_BUNDLE_ID}.${lower}"
  MACOSX_FRAMEWORK_BUNDLE_VERSION "${PROJECT_VERSION}" MACOSX_FRAMEWORK_SHORT_VERSION_STRING "${PROJECT_VERSION}"
  INSTALL_NAME_DIR "@rpath" BUILD_WITH_INSTALL_NAME_DIR TRUE
  BUILD_RPATH "@loader_path/..;@executable_path/Frameworks")
endfunction()
cane_ios_framework(SafeHouseCore SafeHouseCore)

# ImGui, glad and the GLFW API on SDL (platform/ios/glfw_sdl_ios.cpp), linked into each program that draws
set(imgui_root "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/include/imgui")
add_library(cane_ui STATIC ${imgui_root}/imgui.cpp ${imgui_root}/imgui_draw.cpp ${imgui_root}/imgui_tables.cpp
 ${imgui_root}/imgui_widgets.cpp ${imgui_root}/imgui_impl_glfw.cpp ${imgui_root}/imgui_impl_opengl3.cpp
 ${imgui_root}/ImGuiFileDialog.cpp Sly1-Release/glad.c platform/ios/glfw_sdl_ios.cpp)
target_link_libraries(cane_ui PUBLIC cane_platform cane_sdl "-framework OpenGLES" "-framework QuartzCore" ${CMAKE_DL_LIBS})
# The games and the launcher link "glfw" (Sly2-Release/CMakeLists.txt, cmake/prompts.cmake, platform/metal)
add_library(glfw ALIAS cane_ui)

# Each program's entry (platform/ios/program_entry.cpp): its main under another name, called by the bridge.
# (Sly2-Release/CMakeLists.txt does the same for Sly 2.)
function(cane_ios_program target main_source entry role)
 target_sources(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/platform/ios/program_entry.cpp")
 set_property(SOURCE "${main_source}" APPEND PROPERTY COMPILE_DEFINITIONS "main=${entry}")
 target_compile_definitions(${target} PRIVATE "CANE_IOS_ENTRY=${entry}" "CANE_IOS_ROLE=\"${role}\"")
endfunction()

# CHD disc images (platform/disc_image.h), as on the desktop
include(cmake/chd.cmake)
target_link_libraries(cane_chd PUBLIC cane_platform)

# ---- CaneExtractor.framework
add_library(cane-extractor SHARED "File Extractor/main.cpp")
cane_ios_program(cane-extractor "${CMAKE_CURRENT_SOURCE_DIR}/File Extractor/main.cpp" cane_extractor_main extractor)
target_link_libraries(cane-extractor PRIVATE cane_platform cane_chd cane_sdl Threads::Threads)
cane_ios_framework(cane-extractor CaneExtractor)

# ---- SafeHouseLauncher.framework (as cmake/android.cmake's launcher)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/generated/launcher_font.h" COMMAND ${CMAKE_COMMAND}
 -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/launcher/assets/AtkinsonHyperlegible-Regular.ttf
 -DOUTPUT=${CMAKE_BINARY_DIR}/generated/launcher_font.h -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-font.cmake
 DEPENDS launcher/assets/AtkinsonHyperlegible-Regular.ttf cmake/embed-font.cmake VERBATIM)
add_library(cane-launcher SHARED launcher/main.cpp "${CMAKE_BINARY_DIR}/generated/launcher_font.h")
cane_ios_program(cane-launcher "${CMAKE_CURRENT_SOURCE_DIR}/launcher/main.cpp" cane_launcher_main launcher)
cane_ios_framework(cane-launcher SafeHouseLauncher)
function(cane_ios_embed_image target source symbol header)
 add_custom_command(OUTPUT "${header}" COMMAND ${CMAKE_COMMAND} -DSOURCE=${source} -DOUTPUT=${header} -DSYMBOL=${symbol}
  -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-image.cmake DEPENDS "${source}" ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-image.cmake VERBATIM)
 target_sources(${target} PRIVATE "${header}")
endfunction()
foreach(image logo icon)
 cane_ios_embed_image(cane-launcher "${CMAKE_CURRENT_SOURCE_DIR}/launcher/assets/safehouse-${image}.png" cane_${image}
  "${CMAKE_BINARY_DIR}/generated/launcher_${image}.h")
endforeach()
foreach(developer fridinator theclub654 menaman123 thematuu nagaa95)
 foreach(kind "" "-silhouette")
  string(REPLACE "-" "_" symbol "cane_developer_${developer}${kind}")
  cane_ios_embed_image(cane-launcher "${CMAKE_CURRENT_SOURCE_DIR}/launcher/assets/developers/${developer}${kind}.png" ${symbol}
   "${CMAKE_BINARY_DIR}/generated/launcher_developer_${developer}${kind}.h")
 endforeach()
endforeach()
foreach(game sly1 sly2 sly3 sly4 hackpack)
 cane_ios_embed_image(cane-launcher "${CMAKE_CURRENT_SOURCE_DIR}/launcher/assets/games/${game}-logo.png" cane_game_${game}
  "${CMAKE_BINARY_DIR}/generated/launcher_game_${game}.h")
endforeach()
foreach(game sly1 sly2 collection collection_adaptive)
 if(game STREQUAL "collection_adaptive")
  set(image_file "collection-adaptive")
 else()
  set(image_file "${game}-logo")
 endif()
 cane_ios_embed_image(cane-launcher "${CMAKE_CURRENT_SOURCE_DIR}/launcher/assets/shortcut/${image_file}.png" cane_shortcut_${game}
  "${CMAKE_BINARY_DIR}/generated/launcher_shortcut_${game}.h")
endforeach()
set(SAFEHOUSE_VERSION "" CACHE STRING "Release version (x.y.z); empty for development builds")
set(safehouse_build_date "${SAFEHOUSE_BUILD_DATE}")
set(safehouse_commit "${CANE_GIT_HASH}")
find_program(GIT_EXECUTABLE git)
if(NOT safehouse_commit AND GIT_EXECUTABLE)
 execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --short=8 HEAD
  OUTPUT_VARIABLE safehouse_commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
configure_file(cmake/safehouse_version.h.in "${CMAKE_BINARY_DIR}/generated/safehouse_version.h" @ONLY)
target_include_directories(cane-launcher BEFORE PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(cane-launcher PRIVATE cane_ui cane_chd Threads::Threads)
# miniz: the Collection Style Launcher's PS3 menu files (launcher/collection/ps3menu.h, platform/ps3disc.h)
target_sources(cane-launcher PRIVATE Libraries/miniz/miniz.c)
target_include_directories(cane-launcher PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/miniz")

# ---- Sly2Release.framework (Sly2-Release/CMakeLists.txt's CANE_IOS branches; Metal: platform/metal)
# FAudio 25.02 (as the other platforms), static, on SafeHouseCore's SDL: SDL2_INCLUDE_DIRS / SDL2_LIBRARIES are
# FAudio's own way to be given an SDL (empty libraries: the programs link SafeHouseCore themselves)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)   # (FAudio static: it lives inside Sly 2's framework)
set(SDL2_INCLUDE_DIRS "${cane_sdl_includes}")
set(SDL2_LIBRARIES "")
if(NOT FETCHCONTENT_SOURCE_DIR_FAUDIO AND EXISTS "$ENV{HOME}/Documents/tools/projectcane-deps/FAudio-25.02/CMakeLists.txt")
 set(FETCHCONTENT_SOURCE_DIR_FAUDIO "$ENV{HOME}/Documents/tools/projectcane-deps/FAudio-25.02")
endif()
FetchContent_Declare(faudio URL https://codeload.github.com/FNA-XNA/FAudio/tar.gz/refs/tags/25.02
 URL_HASH SHA256=eab667d231e5036b0e5c3c6f89fb4ea792284493d46161bc056a88a6dadc2683)
FetchContent_MakeAvailable(faudio)
# (SDL's generated headers, SDL_config.h among them, exist once its target ran its steps)
add_dependencies(FAudio-static SDL2-static)
if(NOT CANE_FFMPEG_ROOT)
 message(FATAL_ERROR "iOS: set CANE_FFMPEG_ROOT to the FFmpeg built by scripts/ios/ffmpeg.sh for this SDK")
endif()
add_subdirectory(platform/metal)
add_subdirectory(Sly2-Release)
cane_ios_framework(Sly2-Release Sly2Release)
target_include_directories(Sly2-Release PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/platform/apple")

include(cmake/prompts.cmake OPTIONAL)
include(cmake/launcher_ui.cmake OPTIONAL)

# ---- SafeHouse (the executable of SafeHouse.app)
add_executable(SafeHouse platform/ios/main.mm)
target_include_directories(SafeHouse PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/platform/ios" ${cane_sdl_includes})
target_compile_definitions(SafeHouse PRIVATE CANE_IOS)
target_compile_options(SafeHouse PRIVATE -fobjc-arc)
target_link_libraries(SafeHouse PRIVATE SafeHouseCore "-framework UIKit" "-framework Foundation")
# (a plain executable here: scripts/ios/package.py builds the .app around it)
set_target_properties(SafeHouse PROPERTIES MACOSX_BUNDLE FALSE BUILD_RPATH "@executable_path/Frameworks")
# (built before the app is assembled: the frameworks it loads at run time)
add_dependencies(SafeHouse cane-launcher cane-extractor Sly2-Release)
