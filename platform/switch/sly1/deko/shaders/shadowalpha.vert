#version 430 core
// Sly1-Release/shadow.cpp's kShadowAlphaVertexSource, for the Switch's offline set (cmake/switch_shaders_sly1.py): the
// game compiles it at run time elsewhere. Keep the two in step.
void main()
{
    // One triangle that covers the viewport.
    vec2 pos = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
