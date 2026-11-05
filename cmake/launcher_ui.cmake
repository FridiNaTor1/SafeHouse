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
