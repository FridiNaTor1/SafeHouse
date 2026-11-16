# The Khronos OpenXR loader (Apache-2.0; licenses/openxr/README.txt) for Sly 2's VR on Linux and Windows, built from the
# pinned OpenXR-SDK release and installed beside the games: lib/libopenxr_loader.so.1 (found through Sly2-Release's
# $ORIGIN/../lib run path when vr.cpp opens it) or bin/openxr_loader.dll. Most systems have no loader of their own
# (SteamOS, the Steam Frame, Windows: the runtimes do not install one), so without it --vr / "Play in VR" would find
# no runtime. Only the loader is built (no API layers, tests or samples); its jsoncpp is the SDK's own copy.
#   CANE_OPENXR_LOADER=OFF       leave it out (then the system's loader, as before)
#   CANE_OPENXR_SDK_SOURCE=DIR   an extracted OpenXR-SDK-release-1.1.63 instead of the download (offline builds)
include(ExternalProject)
set(cane_openxr_version 1.1.63)
set(CANE_OPENXR_SDK_SOURCE "" CACHE PATH "An extracted OpenXR-SDK-release-${cane_openxr_version} (else it is downloaded)")
set(cane_openxr_prefix "${CMAKE_BINARY_DIR}/openxr-loader")
if(CANE_OPENXR_SDK_SOURCE)
 # (copied into the build folder, so the MinGW patch below never touches the given folder)
 set(cane_openxr_source URL "${CANE_OPENXR_SDK_SOURCE}")
else()
 set(cane_openxr_source
  URL "https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-${cane_openxr_version}.tar.gz"
  URL_HASH SHA256=5b8dac0608f49498fe8e95f562fe6bcf0bc781e561f137348a8c251d06a88b6f
  DOWNLOAD_NAME "OpenXR-SDK-release-${cane_openxr_version}.tar.gz")
 if(NOT CMAKE_VERSION VERSION_LESS 3.24)
  list(APPEND cane_openxr_source DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
 endif()
endif()
set(cane_openxr_args
 -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR> -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_BINDIR=bin
 -DBUILD_TESTS=OFF -DBUILD_API_LAYERS=OFF -DBUILD_CONFORMANCE_TESTS=OFF -DBUILD_SDK_TESTS=OFF -DBUILD_LOADER=ON
 -DDYNAMIC_LOADER=ON -DBUILD_WITH_SYSTEM_JSONCPP=OFF -DBUILD_FORCE_GENERATION=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
 "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
# (the cross builds: the same toolchain file and its settings)
if(CMAKE_TOOLCHAIN_FILE)
 get_filename_component(cane_openxr_toolchain "${CMAKE_TOOLCHAIN_FILE}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
 list(APPEND cane_openxr_args "-DCMAKE_TOOLCHAIN_FILE=${cane_openxr_toolchain}")
endif()
foreach(var CANE_MINGW_ROOT CANE_MINGW_ARCH CMAKE_C_COMPILER_LAUNCHER CMAKE_CXX_COMPILER_LAUNCHER CMAKE_SYSROOT)
 if(${var})
  list(APPEND cane_openxr_args "-D${var}=${${var}}")
 endif()
endforeach()
if(WIN32)
 # (LLVM/MinGW: it uses the libc++ / libunwind DLLs the games use, which scripts/package.py bundles. MinGW names it
 # libopenxr_loader.dll; it is installed as openxr_loader.dll, the name vr.cpp loads.)
 if(MINGW)
  set(cane_openxr_file "${cane_openxr_prefix}/bin/libopenxr_loader.dll")
  set(cane_openxr_patch PATCH_COMMAND ${CMAKE_COMMAND} -DSOURCE=<SOURCE_DIR> -P "${CMAKE_CURRENT_LIST_DIR}/openxr-loader-mingw.cmake")
 else()
  set(cane_openxr_file "${cane_openxr_prefix}/bin/openxr_loader.dll")
 endif()
else()
 set(cane_openxr_file "${cane_openxr_prefix}/lib/libopenxr_loader.so.${cane_openxr_version}")
endif()
ExternalProject_Add(cane-openxr-loader
 ${cane_openxr_source}
 ${cane_openxr_patch}
 PREFIX "${cane_openxr_prefix}/build"
 INSTALL_DIR "${cane_openxr_prefix}"
 CMAKE_ARGS ${cane_openxr_args}
 BUILD_BYPRODUCTS "${cane_openxr_file}"
 # (the SDK's own build of the loader target only; its install puts the library, headers and CMake files in the prefix)
 BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target openxr_loader --config Release
 LOG_CONFIGURE TRUE LOG_BUILD TRUE LOG_INSTALL TRUE LOG_OUTPUT_ON_FAILURE TRUE)
if(WIN32)
 install(FILES "${cane_openxr_file}" DESTINATION bin RENAME openxr_loader.dll)
 # (development runs: beside build/bin/Sly2-Release.exe, where LoadLibrary looks first)
 ExternalProject_Add_Step(cane-openxr-loader copy-to-bin DEPENDEES install
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${cane_openxr_file}" "${CMAKE_BINARY_DIR}/bin/openxr_loader.dll")
else()
 install(FILES "${cane_openxr_file}" DESTINATION lib RENAME libopenxr_loader.so.1)
endif()
# jsoncpp (MIT), compiled into the loader
ExternalProject_Get_Property(cane-openxr-loader SOURCE_DIR)
install(FILES "${SOURCE_DIR}/src/external/jsoncpp/LICENSE" DESTINATION licenses/openxr RENAME jsoncpp-LICENSE.txt OPTIONAL)
