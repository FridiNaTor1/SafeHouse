#version 430 core
// The GL layer's masked clears (gl_vulkan.cpp ClearMasked): one triangle over the frame, at the clear's depth.
uniform float depth;
void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, depth, 1.0);
}
