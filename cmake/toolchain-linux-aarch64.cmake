# Cross build for 64-bit ARM Linux from an x86-64 Ubuntu 22.04 with the arm64 multiarch packages installed.
#   cmake -S . -B build/linux-aarch64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-linux-aarch64.cmake
# `cmake --install` also needs CANE_OBJDUMP=aarch64-linux-gnu-objdump in the environment, and CANE_RUNTIME_LIBDIRS=<FFmpeg>/lib
# (cmake/runtime-deps.cmake).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
