# The Android build (android/README.md): the launcher, the extractor and Sly 2 as JNI shared libraries for the
# org.safehouse.android app. Included from CMakeLists.txt right after project() when the NDK toolchain is used; the
# desktop targets below it are never configured for Android. Adapted from ThiefRush's cmake/android.cmake.
# SDL2 replaces GLFW (platform/android/glfw_sdl.cpp keeps the GLFW API), OpenGL ES 3.2 replaces desktop GL
# (platform/android/gles_sly2.cpp), and the activities own the processes (platform/android/android_app.cpp).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(NOT CMAKE_BUILD_TYPE)
 set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
if(NOT ANDROID_ABI MATCHES "^(arm64-v8a|x86_64)$")
 message(FATAL_ERROR "SafeHouse Android supports arm64-v8a and the x86_64 emulator ABI")
endif()
if(ANDROID_PLATFORM_LEVEL LESS 26)
 message(FATAL_ERROR "SafeHouse Android requires API 26 or newer")
endif()
include(FetchContent)
find_package(Threads REQUIRED)

# ---- Locked NDK dependencies (scripts/android/bootstrap-dependencies.py)
set(CANE_ANDROID_DEPENDENCY_ROOT "${CMAKE_SOURCE_DIR}/build/android-deps/${ANDROID_ABI}/install"
 CACHE PATH "Native Android dependency installation for this ABI (SDL2, FAudio, FFmpeg)")
foreach(required lib/libSDL2.so lib/libFAudio.a lib/libavcodec.so lib/libavutil.so lib/libswscale.so)
 if(NOT EXISTS "${CANE_ANDROID_DEPENDENCY_ROOT}/${required}")
  message(FATAL_ERROR "Android ${ANDROID_ABI} dependency is missing: ${required}. Run python3 scripts/android/bootstrap-dependencies.py --abi ${ANDROID_ABI} first, or set CANE_ANDROID_DEPENDENCY_ROOT.")
 endif()
endforeach()
list(PREPEND CMAKE_PREFIX_PATH "${CANE_ANDROID_DEPENDENCY_ROOT}")
find_package(SDL2 2.30.11 EXACT CONFIG REQUIRED NO_DEFAULT_PATH
 PATHS "${CANE_ANDROID_DEPENDENCY_ROOT}/lib/cmake/SDL2" NO_CMAKE_FIND_ROOT_PATH)
add_library(cane_android_ffmpeg INTERFACE)
foreach(component avformat avcodec avutil swscale)
 add_library(cane_android_${component} SHARED IMPORTED GLOBAL)
 set_target_properties(cane_android_${component} PROPERTIES
  IMPORTED_LOCATION "${CANE_ANDROID_DEPENDENCY_ROOT}/lib/lib${component}.so"
  INTERFACE_INCLUDE_DIRECTORIES "${CANE_ANDROID_DEPENDENCY_ROOT}/include")
 target_link_libraries(cane_android_ffmpeg INTERFACE cane_android_${component})
endforeach()
add_library(FAudio-static STATIC IMPORTED GLOBAL)
set_target_properties(FAudio-static PROPERTIES
 IMPORTED_LOCATION "${CANE_ANDROID_DEPENDENCY_ROOT}/lib/libFAudio.a"
 INTERFACE_INCLUDE_DIRECTORIES "${CANE_ANDROID_DEPENDENCY_ROOT}/include"
 INTERFACE_LINK_LIBRARIES "SDL2::SDL2;log;android;m")

# ---- Shared platform layer
add_library(cane_platform INTERFACE)
target_include_directories(cane_platform BEFORE INTERFACE
 "${CMAKE_CURRENT_SOURCE_DIR}/platform/android"
 "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/include"
 "${CMAKE_CURRENT_SOURCE_DIR}/platform")
target_compile_definitions(cane_platform INTERFACE CANE_ANDROID SDL_MAIN_HANDLED GLFW_INCLUDE_NONE IMGUI_IMPL_OPENGL_ES3
 IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(cane_platform INTERFACE log android GLESv3 EGL)
# The PS2 code expects x86's signed char and unfused multiply-adds (as the aarch64 Linux build, CMakeLists.txt).
target_compile_options(cane_platform INTERFACE -fsigned-char -ffp-contract=off)
set(imgui_root "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/include/imgui")
add_library(cane_ui STATIC ${imgui_root}/imgui.cpp ${imgui_root}/imgui_draw.cpp ${imgui_root}/imgui_tables.cpp
 ${imgui_root}/imgui_widgets.cpp ${imgui_root}/imgui_impl_glfw.cpp ${imgui_root}/imgui_impl_opengl3.cpp
 ${imgui_root}/ImGuiFileDialog.cpp Sly1-Release/glad.c
 platform/android/glfw_sdl.cpp platform/android/android_app.cpp platform/android/gles_sly2.cpp)
target_link_libraries(cane_ui PUBLIC cane_platform SDL2::SDL2 ${CMAKE_DL_LIBS})
# The games and the launcher link "glfw" (Sly2-Release/CMakeLists.txt, cmake/prompts.cmake): the SDL shim.
add_library(glfw ALIAS cane_ui)

# CHD disc images (platform/disc_image.h): cane_chd, as on the desktop
include(cmake/chd.cmake)
target_link_libraries(cane_chd PUBLIC cane_platform)

# ---- cane-extractor (libcane-extractor.so; the extractor service runs its entry point)
add_library(cane-extractor SHARED "File Extractor/main.cpp" platform/android/native_entry.cpp)
set_source_files_properties("File Extractor/main.cpp" PROPERTIES COMPILE_DEFINITIONS main=cane_extractor_implementation)
target_compile_definitions(cane-extractor PRIVATE CANE_ANDROID_EXTRACTOR_ENTRY)
target_link_libraries(cane-extractor PRIVATE cane_ui cane_chd Threads::Threads)

# ---- The launcher (libsafehouse.so)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/generated/launcher_font.h" COMMAND ${CMAKE_COMMAND}
 -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/launcher/assets/AtkinsonHyperlegible-Regular.ttf
 -DOUTPUT=${CMAKE_BINARY_DIR}/generated/launcher_font.h -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-font.cmake
 DEPENDS launcher/assets/AtkinsonHyperlegible-Regular.ttf cmake/embed-font.cmake VERBATIM)
add_library(cane-launcher SHARED launcher/main.cpp platform/android/native_entry.cpp
 "${CMAKE_BINARY_DIR}/generated/launcher_font.h")
set_target_properties(cane-launcher PROPERTIES OUTPUT_NAME safehouse)
set_source_files_properties(launcher/main.cpp PROPERTIES COMPILE_DEFINITIONS main=cane_launcher_main)
target_compile_definitions(cane-launcher PRIVATE CANE_ANDROID_LAUNCHER_ENTRY)
foreach(image logo icon)
 set(image_header "${CMAKE_BINARY_DIR}/generated/launcher_${image}.h")
 set(image_source "${CMAKE_CURRENT_SOURCE_DIR}/launcher/assets/safehouse-${image}.png")
 add_custom_command(OUTPUT "${image_header}" COMMAND ${CMAKE_COMMAND} -DSOURCE=${image_source} -DOUTPUT=${image_header}
  -DSYMBOL=cane_${image} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-image.cmake DEPENDS "${image_source}" cmake/embed-image.cmake VERBATIM)
 target_sources(cane-launcher PRIVATE "${image_header}")
endforeach()
foreach(game sly1 sly2 sly3 sly4 hackpack)
 set(image_header "${CMAKE_BINARY_DIR}/generated/launcher_game_${game}.h")
 set(image_source "${CMAKE_CURRENT_SOURCE_DIR}/launcher/assets/games/${game}-logo.png")
 add_custom_command(OUTPUT "${image_header}" COMMAND ${CMAKE_COMMAND} -DSOURCE=${image_source} -DOUTPUT=${image_header}
  -DSYMBOL=cane_game_${game} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-image.cmake DEPENDS "${image_source}" cmake/embed-image.cmake VERBATIM)
 target_sources(cane-launcher PRIVATE "${image_header}")
endforeach()
foreach(game sly1 sly2)
 set(image_header "${CMAKE_BINARY_DIR}/generated/launcher_shortcut_${game}.h")
 set(image_source "${CMAKE_CURRENT_SOURCE_DIR}/launcher/assets/shortcut/${game}-logo.png")
 add_custom_command(OUTPUT "${image_header}" COMMAND ${CMAKE_COMMAND} -DSOURCE=${image_source} -DOUTPUT=${image_header}
  -DSYMBOL=cane_shortcut_${game} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-image.cmake DEPENDS "${image_source}" cmake/embed-image.cmake VERBATIM)
 target_sources(cane-launcher PRIVATE "${image_header}")
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
# miniz: the Collection Style Launcher's PS3 menu files (launcher/collection/assets.h, platform/ps3disc.h)
target_sources(cane-launcher PRIVATE Libraries/miniz/miniz.c)
target_include_directories(cane-launcher PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/miniz")

# ---- Sly 2 (libSly2-Release.so; Sly2-Release/CMakeLists.txt's ANDROID branches)
add_subdirectory(Sly2-Release)

include(cmake/prompts.cmake OPTIONAL)
include(cmake/launcher_ui.cmake OPTIONAL)
foreach(target cane-launcher cane-extractor Sly2-Release)
 target_link_options(${target} PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,--no-undefined"
  "-Wl,--wrap=exit" "-Wl,--wrap=_exit" "-Wl,--wrap=_Exit")   # (native_entry.cpp: the exit code reaches the launcher)
endforeach()
