# Inspect build artifacts together before copying any library, avoiding
# resolution conflicts between newly staged libraries and their originals.
# A cross build reads the target's ELF files with the target's objdump (cmake/toolchain-linux-aarch64.cmake).
if(DEFINED ENV{CANE_OBJDUMP})
 set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "linux+elf")
 set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "objdump")
 set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "$ENV{CANE_OBJDUMP}")
 set(CMAKE_OBJDUMP "$ENV{CANE_OBJDUMP}")
endif()
file(GET_RUNTIME_DEPENDENCIES
 EXECUTABLES ${cane_runtime_executables}
 DIRECTORIES $ENV{CANE_RUNTIME_LIBDIRS}
 RESOLVED_DEPENDENCIES_VAR deps
 PRE_EXCLUDE_REGEXES "^linux-vdso" "^ld-linux" "^libstdc" "^libgcc_s" "^libc[.]so" "^libm[.]so" "^libdl[.]so" "^libpthread[.]so" "^librt[.]so" "^libGL" "^libEGL" "^libOpenGL")
foreach(dep IN LISTS deps)
 file(INSTALL "${dep}" DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" FOLLOW_SYMLINK_CHAIN)
endforeach()
