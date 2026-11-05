# Controller prompt atlases (Kenney Input Prompts 1.5A, CC0; packed by
# tools/pack_prompts.py into assets/prompts) and SDL_GameControllerDB (zlib;
# Libraries/gamecontrollerdb), embedded into the game and the launcher.
# Included at the end of CMakeLists.txt, after every target exists.
set(cane_prompt_source "${CMAKE_CURRENT_SOURCE_DIR}/assets/prompts")
set(cane_prompt_families keyboard playstation xbox nintendo steamdeck steamcontroller ps4 ps5 steamcontroller2026 devices)
set(cane_gamecontrollerdb "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/gamecontrollerdb/gamecontrollerdb.txt")
if(TARGET glfw AND EXISTS "${cane_prompt_source}/prompt_atlas_rects.inc" AND EXISTS "${cane_gamecontrollerdb}")
 set(cane_prompt_generated "${CMAKE_BINARY_DIR}/generated/prompts")
 file(MAKE_DIRECTORY "${cane_prompt_generated}")
 set(cane_prompt_headers "")
 foreach(family IN LISTS cane_prompt_families)
  set(image "${cane_prompt_source}/prompts_${family}.png")
  set(header "${cane_prompt_generated}/prompts_${family}_png.h")
  add_custom_command(OUTPUT "${header}" COMMAND ${CMAKE_COMMAND} -DSOURCE=${image} -DOUTPUT=${header} -DSYMBOL=cane_prompts_${family} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-image.cmake DEPENDS "${image}" cmake/embed-image.cmake VERBATIM)
  list(APPEND cane_prompt_headers "${header}")
 endforeach()
 set(header "${cane_prompt_generated}/gamecontrollerdb_txt.h")
 add_custom_command(OUTPUT "${header}" COMMAND ${CMAKE_COMMAND} -DSOURCE=${cane_gamecontrollerdb} -DOUTPUT=${header} -DSYMBOL=cane_gamecontrollerdb -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-image.cmake DEPENDS "${cane_gamecontrollerdb}" cmake/embed-image.cmake VERBATIM)
 list(APPEND cane_prompt_headers "${header}")

 # The embedded data and a private PNG decoder, compiled once.
 add_library(cane_prompts_data OBJECT "${cane_prompt_source}/prompt_data.cpp" ${cane_prompt_headers})
 target_link_libraries(cane_prompts_data PRIVATE cane_platform glfw)
 target_include_directories(cane_prompts_data PRIVATE "${cane_prompt_source}" "${cane_prompt_generated}")
 target_compile_definitions(cane_prompts_data PRIVATE CANE_HAVE_PROMPT_ATLAS=1 CANE_HAVE_GAMECONTROLLERDB=1 GLFW_INCLUDE_NONE)
 if(MSVC)
  set_property(TARGET cane_prompts_data PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
 endif()

 # Link this to get button_prompts.h and gamepad_mappings.h with real data.
 add_library(cane_prompts INTERFACE)
 target_sources(cane_prompts INTERFACE $<TARGET_OBJECTS:cane_prompts_data>)
 target_include_directories(cane_prompts INTERFACE "${cane_prompt_source}")
 target_compile_definitions(cane_prompts INTERFACE CANE_HAVE_PROMPT_ATLAS=1 CANE_HAVE_GAMECONTROLLERDB=1)
 foreach(target Sly1-Release cane-launcher)
  if(TARGET ${target})
   target_link_libraries(${target} PRIVATE cane_prompts)
   add_dependencies(${target} cane_prompts_data)
  endif()
 endforeach()

 if(BUILD_TESTING AND TARGET cane_ui AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/button_prompts_tests.cpp")
  add_executable(cane-button-prompts-tests tests/button_prompts_tests.cpp)
  target_link_libraries(cane-button-prompts-tests PRIVATE cane_prompts cane_ui Threads::Threads)
  target_include_directories(cane-button-prompts-tests BEFORE PRIVATE "${glfw_SOURCE_DIR}/include")
  add_dependencies(cane-button-prompts-tests cane_prompts_data)
  if(MSVC)
   set_property(TARGET cane-button-prompts-tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  add_test(NAME button-prompts COMMAND cane-button-prompts-tests --source "${CMAKE_CURRENT_SOURCE_DIR}")
  # Renders the atlases, prompt bars and the on-screen keyboard for review
  # (needs a display; skipped without one).
  add_test(NAME button-prompts-render COMMAND cane-button-prompts-tests --source "${CMAKE_CURRENT_SOURCE_DIR}" --render "${CMAKE_BINARY_DIR}/prompt-review")
  set_tests_properties(button-prompts-render PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
 endif()
 # Optional: -DCANE_KENNEY_ZIP=<kenney_input-prompts_1.5.zip> checks that the
 # committed atlases are exactly what tools/pack_prompts.py produces (Pillow).
 set(CANE_KENNEY_ZIP "" CACHE FILEPATH "Kenney Input Prompts 1.5 zip for the atlas reproducibility test")
 if(BUILD_TESTING AND CANE_KENNEY_ZIP)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME prompt-atlas-reproducible COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_prompts.py" --zip "${CANE_KENNEY_ZIP}" --check)
 endif()
endif()
