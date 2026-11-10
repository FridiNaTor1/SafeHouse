# Toolchain for the Nintendo Switch build (devkitPro devkitA64 + libnx + deko3d).
#
#   cmake --preset switch-release
#   cmake --build --preset switch-release
#
# DEVKITPRO comes from the environment (the devkitPro installer sets it); without it the usual locations are tried
# (C:/devkitPro, C:/msys64/opt/devkitpro, /opt/devkitpro).
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(NINTENDO_SWITCH TRUE)

set(DEVKITPRO "$ENV{DEVKITPRO}" CACHE PATH "devkitPro installation")
if(DEVKITPRO MATCHES "^/opt/devkitpro" AND CMAKE_HOST_WIN32)
  # (an MSYS2 shell's DEVKITPRO is a POSIX path: map it to the Windows one)
  set(DEVKITPRO "")
endif()
if(NOT DEVKITPRO OR NOT EXISTS "${DEVKITPRO}/devkitA64")
  foreach(candidate "C:/devkitPro" "C:/msys64/opt/devkitpro" "/opt/devkitpro")
    if(EXISTS "${candidate}/devkitA64")
      set(DEVKITPRO "${candidate}" CACHE PATH "devkitPro installation" FORCE)
      break()
    endif()
  endforeach()
endif()
if(NOT EXISTS "${DEVKITPRO}/devkitA64")
  message(FATAL_ERROR "devkitA64 not found: install devkitPro (switch-dev) or set DEVKITPRO")
endif()

set(CANE_HOST_SUFFIX "")
if(CMAKE_HOST_WIN32)
  set(CANE_HOST_SUFFIX ".exe")
endif()
set(CMAKE_C_COMPILER "${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-gcc${CANE_HOST_SUFFIX}")
set(CMAKE_CXX_COMPILER "${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-g++${CANE_HOST_SUFFIX}")
set(CMAKE_ASM_COMPILER "${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-gcc${CANE_HOST_SUFFIX}")
set(CMAKE_AR "${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-gcc-ar${CANE_HOST_SUFFIX}" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-gcc-ranlib${CANE_HOST_SUFFIX}" CACHE FILEPATH "" FORCE)
set(CMAKE_NM "${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-gcc-nm${CANE_HOST_SUFFIX}" CACHE FILEPATH "" FORCE)
set(CMAKE_OBJCOPY "${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-objcopy${CANE_HOST_SUFFIX}" CACHE FILEPATH "" FORCE)
set(CMAKE_STRIP "${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-strip${CANE_HOST_SUFFIX}" CACHE FILEPATH "" FORCE)
set(CMAKE_EXECUTABLE_SUFFIX ".elf")

set(CMAKE_FIND_ROOT_PATH "${DEVKITPRO}/devkitA64/aarch64-none-elf" "${DEVKITPRO}/portlibs/switch" "${DEVKITPRO}/libnx")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The libnx ABI: A57 tuning, software TLS pointer, position independent (the NRO loader relocates).
set(CANE_SWITCH_ARCH "-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE")
set(CMAKE_C_FLAGS_INIT "${CANE_SWITCH_ARCH} -ftls-model=local-exec -ffunction-sections -fdata-sections -D__SWITCH__ -isystem ${DEVKITPRO}/libnx/include")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_C_FLAGS_INIT}")
set(CMAKE_ASM_FLAGS_INIT "${CANE_SWITCH_ARCH} -x assembler-with-cpp")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-specs=${DEVKITPRO}/libnx/switch.specs -L${DEVKITPRO}/libnx/lib -L${DEVKITPRO}/portlibs/switch/lib -Wl,--gc-sections")
