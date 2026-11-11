#version 430 core
// Particles (render kind 12): quads already in clip space (emitter.cpp ExpandParticles, the VU1 programs
// 0x25A0C0.. as runtime/native_blip_geometry.cpp reconstructs them); colours in GS units.
layout (location = 0) in vec4 aClip;
layout (location = 1) in vec2 aUv;
layout (location = 2) in float aLayer;
layout (location = 3) in vec4 aRgba;
out vec2 vUv;
out float vLayer;
out vec4 vRgba;
#ifdef CANE_MULTIVIEW
// (Vulkan VR, both eyes in one pass: emitter.cpp DrawParticles writes the second eye's corners after the vertices)
layout(std430, binding = 10) readonly buffer CaneClip1 { vec4 caneClip1[]; };
#endif
void main()
{
    gl_Position = aClip;
#ifdef CANE_MULTIVIEW
    if (gl_ViewIndex != 0) gl_Position = caneClip1[gl_VertexID];
#endif
    vUv = aUv;
    vLayer = aLayer;
    vRgba = aRgba;
}
