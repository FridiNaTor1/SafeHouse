# Windows on ARM (ARM64) with LLVM/MinGW, from Linux or natively on an ARM64 PC: the windows-arm64 preset, after
#   scripts/ffmpeg-minimal.sh windows-arm64 build/ffmpeg-windows-arm64
set(CANE_MINGW_ARCH aarch64)
include("${CMAKE_CURRENT_LIST_DIR}/mingw-toolchain.cmake")
