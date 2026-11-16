# Toolchain for the Nintendo Switch build (devkitPro devkitA64 + libnx + deko3d), and with CANE_SWITCH2 for the Switch 2
# build (cmake/devkitA64-libounce.cmake: libounce + drako3d).
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

# libnx, or libounce for the Switch 2: the root of the headers, libraries and linker specs. The Switch 2 also links the
# Switch portlibs (FFmpeg, zlib), after its own.
if(CANE_SWITCH2)
  set(NINTENDO_SWITCH2 TRUE)
  set(CANE_NX_ROOT "${DEVKITPRO}/libounce")
  if(NOT EXISTS "${CANE_NX_ROOT}/switch2_cpu.mk")
    message(FATAL_ERROR "libounce not found in ${DEVKITPRO}: install ouncebrew's ounce-devkit (https://github.com/ouncebrew)")
  endif()
  # (the Switch 2's CPU flags, from libounce's switch2_cpu.mk)
  file(STRINGS "${CANE_NX_ROOT}/switch2_cpu.mk" cane_switch2_cpu REGEX "^SWITCH2_CPU[ \t]*:=")
  string(REGEX REPLACE "^SWITCH2_CPU[ \t]*:=[ \t]*" "" cane_switch2_cpu "${cane_switch2_cpu}")
  if(NOT cane_switch2_cpu)
    message(FATAL_ERROR "No SWITCH2_CPU in ${CANE_NX_ROOT}/switch2_cpu.mk")
  endif()
  set(CANE_SWITCH_ARCH "${cane_switch2_cpu} -mtp=soft -fPIE")
  set(CANE_SWITCH_DEFINES "-D__SWITCH__ -D__SWITCH2__")
  set(CANE_PORTLIBS "${DEVKITPRO}/portlibs/switch2" "${DEVKITPRO}/portlibs/switch")
else()
  set(CANE_NX_ROOT "${DEVKITPRO}/libnx")
  # The libnx ABI: A57 tuning, software TLS pointer, position independent (the NRO loader relocates).
  set(CANE_SWITCH_ARCH "-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE")
  set(CANE_SWITCH_DEFINES "-D__SWITCH__")
  set(CANE_PORTLIBS "${DEVKITPRO}/portlibs/switch")
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

set(CMAKE_FIND_ROOT_PATH "${DEVKITPRO}/devkitA64/aarch64-none-elf" ${CANE_PORTLIBS} "${CANE_NX_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(cane_portlib_dirs "")
foreach(portlibs IN LISTS CANE_PORTLIBS)
  string(APPEND cane_portlib_dirs " -L${portlibs}/lib")
endforeach()
set(CMAKE_C_FLAGS_INIT "${CANE_SWITCH_ARCH} -ftls-model=local-exec -ffunction-sections -fdata-sections ${CANE_SWITCH_DEFINES} -isystem ${CANE_NX_ROOT}/include")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_C_FLAGS_INIT}")
set(CMAKE_ASM_FLAGS_INIT "${CANE_SWITCH_ARCH} -x assembler-with-cpp")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-specs=${CANE_NX_ROOT}/switch.specs -L${CANE_NX_ROOT}/lib${cane_portlib_dirs} -Wl,--gc-sections")
