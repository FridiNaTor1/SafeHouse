# PATCH_COMMAND of cmake/openxr-loader.cmake for LLVM/MinGW (cmake -DSOURCE=<SDK> -P): the SDK gives every non-MSVC,
# non-Apple compiler a GNU ld --version-script, which lld's COFF driver rejects; MinGW exports through the SDK's own
# openxr-loader.def instead (as MSVC does). Idempotent.
set(file "${SOURCE}/src/loader/CMakeLists.txt")
file(READ "${file}" text)
set(old "    else()\n        set_target_properties(\n            openxr_loader\n            PROPERTIES\n                LINK_FLAGS\n                \"-Wl,--version-script")
set(new "    elseif(MINGW)\n        target_sources(openxr_loader PRIVATE \"\${CMAKE_CURRENT_SOURCE_DIR}/openxr-loader.def\")\n    else()\n        set_target_properties(\n            openxr_loader\n            PROPERTIES\n                LINK_FLAGS\n                \"-Wl,--version-script")
string(FIND "${text}" "elseif(MINGW)" done)
if(done EQUAL -1)
 string(FIND "${text}" "${old}" at)
 if(at EQUAL -1)
  message(FATAL_ERROR "openxr-loader-mingw.cmake: the loader's CMakeLists.txt changed; update the patch")
 endif()
 string(REPLACE "${old}" "${new}" text "${text}")
 file(WRITE "${file}" "${text}")
endif()
