#version 430 core
// VR (vr.cpp): one triangle over the target, uv 0..1 (the HUD panel's copy, the comfort vignette)
out vec2 vUv;
void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
