set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

get_filename_component(CANE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${generated}")
file(GLOB metal_shader_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.metal")
add_custom_command(
  OUTPUT "${generated}/metal_shaders.h"
  COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/shaders -DOUTPUT=${generated}/metal_shaders.h
          -DEXTENSIONS=metal -DFUNCTION=CaneMetalShader -P ${CANE_ROOT}/cmake/embed-shaders.cmake
  DEPENDS ${metal_shader_sources} ${CANE_ROOT}/cmake/embed-shaders.cmake
  VERBATIM)

add_library(cane_gl_metal STATIC
  context.mm
  context_programs.mm
  context_render.mm
  context_resources.mm
  enum_translation.mm
  gl_entry_points.mm
  glsl_interface.mm
  pipeline_cache.mm
  shader_library.mm
  submission_tracker.mm
  transient_allocator.mm
  "${generated}/metal_shaders.h")
target_include_directories(cane_gl_metal
  PUBLIC "${CANE_ROOT}/platform"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${generated}" "${CANE_ROOT}/Libraries/include")
target_compile_options(cane_gl_metal PRIVATE -fobjc-arc -Wall -Wextra)
target_compile_definitions(cane_gl_metal PRIVATE GLFW_INCLUDE_NONE)
target_link_libraries(cane_gl_metal
  PUBLIC glfw "-framework Metal" "-framework QuartzCore" "-framework Cocoa")
