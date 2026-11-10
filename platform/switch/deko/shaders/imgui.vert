#version 430 core
// Dear ImGui's vertices (the installer, platform/switch/installer.cpp): position in display units, UV, colour.
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aUv;
layout (location = 2) in vec4 aColor;   // (unsigned bytes, normalized)
uniform mat4 ProjMtx;
out vec2 vUv;
out vec4 vColor;
void main()
{
    vUv = aUv;
    vColor = aColor;
    gl_Position = ProjMtx * vec4(aPos, 0.0, 1.0);
}
