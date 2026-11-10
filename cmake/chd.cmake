# CHD disc images for the installers (platform/disc_image.h): libchdr, BSD-3-Clause (Libraries/libchdr: the sources of
# github.com/rtissera/libchdr 47d7941 with its LZMA, miniz and zstd decoders and dr_flac), as the static library
# cane_chd. Included by CMakeLists.txt and cmake/Switch.cmake.
set(cane_chdr "${CMAKE_CURRENT_LIST_DIR}/../Libraries/libchdr")
set(cane_chdr_sources
  "${cane_chdr}/deps/lzma-26.02/src/LzmaDec.c" "${cane_chdr}/deps/miniz-3.1.2/miniz.c" "${cane_chdr}/deps/zstd-1.5.7/zstddeclib.c")
foreach(source bitstream cdrom chd codec_avhuff codec_cdfl codec_cdlz codec_cdzl codec_cdzs codec_flac codec_huff codec_lzma
               codec_zlib codec_zstd flac huffman)
  list(APPEND cane_chdr_sources "${cane_chdr}/src/libchdr_${source}.c")
endforeach()
add_library(cane_chd STATIC "${CMAKE_CURRENT_LIST_DIR}/../platform/disc_image.cpp" ${cane_chdr_sources})
target_include_directories(cane_chd PUBLIC "${cane_chdr}/include")
target_compile_features(cane_chd PRIVATE cxx_std_20)
target_compile_definitions(cane_chd PRIVATE WANT_RAW_DATA_SECTOR=1 WANT_SUBCODE=1 VERIFY_BLOCK_CRC=1 LOWRAM_TARGET=0
  MINIZ_NO_ARCHIVE_APIS MINIZ_NO_DEFLATE_APIS MINIZ_NO_STDIO MINIZ_NO_TIME)
# (its miniz renamed apart from Libraries/miniz, which the launcher and Sly 1 link)
foreach(symbol miniz_def_alloc_func miniz_def_free_func miniz_def_realloc_func mz_adler32 mz_crc32 mz_error mz_free mz_inflate
               mz_inflateEnd mz_inflateInit mz_inflateInit2 mz_inflateReset mz_uncompress mz_uncompress2 mz_version
               tinfl_decompress tinfl_decompress_mem_to_callback tinfl_decompress_mem_to_heap tinfl_decompress_mem_to_mem
               tinfl_decompressor_alloc tinfl_decompressor_free)
  target_compile_definitions(cane_chd PRIVATE "${symbol}=cane_chdr_${symbol}")
endforeach()
if(MSVC)
  target_compile_definitions(cane_chd PRIVATE _CRT_SECURE_NO_WARNINGS)
  set_source_files_properties(${cane_chdr_sources} PROPERTIES COMPILE_OPTIONS "/w")
else()
  set_source_files_properties(${cane_chdr_sources} PROPERTIES COMPILE_OPTIONS "-w")
endif()
