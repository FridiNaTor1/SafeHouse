# sly1_pcbrx: Sly 1's level tools as a library, from the Sly1/ submodule (upstream ProjectCane's File Extractor GUI/
# NativeTools: BrxRebuilder turns a decompressed retail level into PCBRX v17, PalExtractor unpacks the European,
# Japanese and Korean discs). cane-extractor calls them in its own process (File Extractor/pcbrx.h) on every platform:
# Android, iOS and the Switch cannot start helper programs. PalExtractor's classes are in namespace cane::pal (a fork
# commit), so both tools link into one library. Included from the top-level CMakeLists.txt, cmake/android.cmake and
# cmake/ios.cmake before cane-extractor.
if(TARGET sly1_pcbrx)
 return()
endif()
set(sly1_native_tools "${CMAKE_CURRENT_SOURCE_DIR}/Sly1/File Extractor GUI/NativeTools")
if(NOT EXISTS "${sly1_native_tools}/PalExtractor/PalIsoExtractor.cpp")
 message(FATAL_ERROR "Sly 1's level tools are missing (${sly1_native_tools}). They are in the Sly1 git submodule: run "
  "`git submodule update --init` (or clone with --recursive).")
endif()
set(sly1_pcbrx_sources "")
foreach(source AssetWalker BinaryInputStream BrxWalker GeometryWalker ObjectWalker OptionWalker SpliceWalker TriangleList)
 list(APPEND sly1_pcbrx_sources "${sly1_native_tools}/BrxRebuilder/${source}.cpp")
endforeach()
foreach(source AssetWalker BinaryInputStream BrxWalker GeometryWalker ObjectWalker OptionWalker PalIsoExtractor SpliceWalker)
 list(APPEND sly1_pcbrx_sources "${sly1_native_tools}/PalExtractor/${source}.cpp")
endforeach()
add_library(sly1_pcbrx STATIC ${sly1_pcbrx_sources})
# (each tool includes its own headers by plain name, found beside the including file: neither folder goes on the
# include path, where the two tools' same-named headers would shadow each other)
target_compile_features(sly1_pcbrx PUBLIC cxx_std_20)
target_compile_definitions(sly1_pcbrx PRIVATE _CRT_SECURE_NO_WARNINGS)
# The tools were written for x86, where char is signed: Arm's unsigned char rebuilt 36 of the 46 USA levels differently
# (an Android install against the desktop's). Floats without fused multiply-adds, as the games.
if(NOT MSVC)
 target_compile_options(sly1_pcbrx PRIVATE -fsigned-char -ffp-contract=off)
endif()
# Users include "BrxRebuilder/BrxWalker.h" and "PalExtractor/PalIsoExtractor.h".
target_include_directories(sly1_pcbrx INTERFACE "${sly1_native_tools}")
