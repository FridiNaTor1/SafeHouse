#pragma once
// Dear ImGui's draw lists through the game's GL entry points (glad), with a program the caller made: for a renderer
// behind the GL calls that has no run-time GLSL compiler (Vulkan through platform/vulkan/gl_vulkan.cpp, whose program
// is platform/vulkan/shaders/imgui.vert / .frag, compiled at build time as "imgui.vert" / "imgui.frag"). Dear ImGui's
// own OpenGL 3 backend compiles its program at run time and loads GL by itself, so it serves OpenGL only.
// Everything it changes in the GL state is put back as it was (the game's state mirrors stay right).
struct ImDrawData;

namespace cane::imgui_gl {
// program: the linked imgui program (0: nothing is drawn)
void Init(unsigned program);
void NewFrame();
void Render(ImDrawData* data);
void Shutdown();
}  // namespace cane::imgui_gl
