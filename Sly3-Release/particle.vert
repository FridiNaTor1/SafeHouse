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
void main()
{
    gl_Position = aClip;
    vUv = aUv;
    vLayer = aLayer;
    vRgba = aRgba;
}
