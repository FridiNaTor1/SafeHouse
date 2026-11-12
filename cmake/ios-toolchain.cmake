# iOS / iPadOS cross build (cmake/ios.cmake, .github/workflows/ios.yml), on a Mac with Xcode:
#   cmake -G Ninja -S . -B build/ios -DCMAKE_TOOLCHAIN_FILE=cmake/ios-toolchain.cmake -DCANE_FFMPEG_ROOT=<ffmpeg>
#   (simulator: -DCMAKE_OSX_SYSROOT=iphonesimulator)
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Oldest iOS / iPadOS the app runs on")
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "")
if(NOT CMAKE_OSX_SYSROOT)
 set(CMAKE_OSX_SYSROOT "iphoneos" CACHE STRING "iphoneos (devices) or iphonesimulator")
endif()
# (FFmpeg and the fetched sources live outside the SDK)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
