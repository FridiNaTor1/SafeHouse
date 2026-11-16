# Launcher UI assets (included at the end of CMakeLists.txt). Embeds the bold
# weight of Atkinson Hyperlegible (SIL OFL, launcher/assets/OFL.txt) as
# cane_font_bold in generated/launcher_font_bold.h. The Regular weight is
# embedded by the main CMakeLists.txt. Without the bold file the launcher
# falls back to the Regular weight (launcher/ui/theme.h).
set(cane_launcher_bold_source "${CMAKE_CURRENT_SOURCE_DIR}/launcher/assets/AtkinsonHyperlegible-Bold.ttf")
if(TARGET cane-launcher AND EXISTS "${cane_launcher_bold_source}")
 set(cane_launcher_bold_header "${CMAKE_BINARY_DIR}/generated/launcher_font_bold.h")
 add_custom_command(OUTPUT "${cane_launcher_bold_header}"
  COMMAND ${CMAKE_COMMAND} -DSOURCE=${cane_launcher_bold_source} -DOUTPUT=${cane_launcher_bold_header} -DSYMBOL=cane_font_bold -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-image.cmake
  DEPENDS "${cane_launcher_bold_source}" cmake/embed-image.cmake VERBATIM)
 target_sources(cane-launcher PRIVATE "${cane_launcher_bold_header}")
endif()
# The launcher's languages (launcher/lang/<code>.json, launcher/l10n.h) as generated/launcher_lang.h, and the Noto
# subsets for their other letters (launcher/assets/l10n, scripts/make-launcher-l10n-fonts.py) as
# generated/launcher_l10n_fonts.h.
if(TARGET cane-launcher)
 file(GLOB cane_launcher_lang_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/launcher/lang/*.json")
 set(cane_launcher_lang_header "${CMAKE_BINARY_DIR}/generated/launcher_lang.h")
 add_custom_command(OUTPUT "${cane_launcher_lang_header}"
  COMMAND ${CMAKE_COMMAND} -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/launcher/lang -DOUTPUT=${cane_launcher_lang_header} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-lang.cmake
  DEPENDS ${cane_launcher_lang_files} cmake/embed-lang.cmake VERBATIM)
 target_sources(cane-launcher PRIVATE "${cane_launcher_lang_header}")
 set(cane_l10n_font_dir "${CMAKE_CURRENT_SOURCE_DIR}/launcher/assets/l10n")
 set(cane_l10n_fonts "${cane_l10n_font_dir}/NotoSans-Regular.subset.ttf;${cane_l10n_font_dir}/NotoSans-Bold.subset.ttf;${cane_l10n_font_dir}/NotoSansCJKjp-Regular.subset.otf;${cane_l10n_font_dir}/NotoSansCJKjp-Bold.subset.otf")
 set(cane_l10n_fonts_present TRUE)
 foreach(f IN LISTS cane_l10n_fonts)
  if(NOT EXISTS "${f}")
   set(cane_l10n_fonts_present FALSE)
  endif()
 endforeach()
 if(cane_l10n_fonts_present)
  set(cane_l10n_fonts_header "${CMAKE_BINARY_DIR}/generated/launcher_l10n_fonts.h")
  add_custom_command(OUTPUT "${cane_l10n_fonts_header}"
   COMMAND ${CMAKE_COMMAND} "-DFILES=${cane_l10n_fonts}" "-DSYMBOLS=cane_l10n_noto_regular;cane_l10n_noto_bold;cane_l10n_cjk_regular;cane_l10n_cjk_bold"
           -DOUTPUT=${cane_l10n_fonts_header} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-files.cmake
   DEPENDS ${cane_l10n_fonts} cmake/embed-files.cmake VERBATIM)
  target_sources(cane-launcher PRIVATE "${cane_l10n_fonts_header}")
 endif()
endif()
