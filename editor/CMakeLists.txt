# The map editor's core (desktop only; notes: Sly2-Release/notes/EDITOR_2026-11-12.md). Linked by the launcher's
# Editor page and by Sly2-Release's --editor (SLY2_EDITOR). The consumers compile miniz themselves (zip export).
get_filename_component(cane_editor_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
add_library(cane_editor_core STATIC "${CMAKE_CURRENT_LIST_DIR}/core/world.cpp" "${CMAKE_CURRENT_LIST_DIR}/core/project.cpp")
target_include_directories(cane_editor_core PUBLIC "${CMAKE_CURRENT_LIST_DIR}" PRIVATE "${cane_editor_root}/Libraries/include" "${cane_editor_root}/Libraries/miniz")
target_include_directories(cane_editor_core PUBLIC "${cane_editor_root}/Libraries/include")
set_target_properties(cane_editor_core PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)

# Model files (phase 4: GLB / FBX import, GLB export): cgltf (MIT, Libraries/cgltf) and ufbx (MIT / public domain,
# Libraries/ufbx). Only Sly2-Release's 3D editor links it (the launcher page does not read models).
if(NOT TARGET cane_editor_model)
  enable_language(C)
  add_library(cane_editor_model STATIC "${CMAKE_CURRENT_LIST_DIR}/core/model.cpp" "${cane_editor_root}/Libraries/ufbx/ufbx.c")
  target_include_directories(cane_editor_model PUBLIC "${CMAKE_CURRENT_LIST_DIR}" "${cane_editor_root}/Libraries/include"
                             PRIVATE "${cane_editor_root}/Libraries/cgltf" "${cane_editor_root}/Libraries/ufbx")
  set_target_properties(cane_editor_model PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
  set_source_files_properties("${cane_editor_root}/Libraries/ufbx/ufbx.c" PROPERTIES COMPILE_OPTIONS "-w")
  if(UNIX OR MINGW)
    target_link_libraries(cane_editor_model PRIVATE m)
  endif()
endif()
