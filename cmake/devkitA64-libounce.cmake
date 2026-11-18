# Toolchain for the Nintendo Switch 2 build: devkitPro's devkitA64 with ouncebrew's libounce and drako3d.
#
#   cmake --preset switch2-release
#   cmake --build --preset switch2-release
set(CANE_SWITCH2 TRUE)
include("${CMAKE_CURRENT_LIST_DIR}/devkitA64-libnx.cmake")
