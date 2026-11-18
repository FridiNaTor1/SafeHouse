# The developers whose pictures the launcher shows (launcher/credits.h kDevelopers, in that order): each one's
# launcher/assets/developers/<name>.png and <name>-silhouette.png. cane_embed_developers(target) embeds them all as one
# generated header, launcher_developers.h (cmake/embed-developers.cmake: cane_developer_pictures[], each picture then
# its silhouette). Used by the desktop, Android and iOS builds.
set(CANE_DEVELOPERS fridinator theclub654 menaman123 thematuu nagaa95 cooperbooperscooper fyrnas maxigasparini)

function(cane_embed_developers target)
 set(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
 set(header "${CMAKE_BINARY_DIR}/generated/launcher_developers.h")
 set(sources "")
 foreach(developer ${CANE_DEVELOPERS})
  list(APPEND sources "${root}/launcher/assets/developers/${developer}.png" "${root}/launcher/assets/developers/${developer}-silhouette.png")
 endforeach()
 string(REPLACE ";" "," names "${CANE_DEVELOPERS}")
 add_custom_command(OUTPUT "${header}"
  COMMAND ${CMAKE_COMMAND} "-DDIR=${root}/launcher/assets/developers" "-DNAMES=${names}" "-DOUTPUT=${header}"
          -P "${root}/cmake/embed-developers.cmake"
  DEPENDS ${sources} "${root}/cmake/embed-developers.cmake" "${root}/cmake/developers.cmake" VERBATIM)
 target_sources(${target} PRIVATE "${header}")
endfunction()
