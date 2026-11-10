#version 430 core
// NOKILL: the variant without DATE (no discard), for when there is none or the stencil holds its mark (render.cpp
// FMarkDate), so the GPU tests depth and stencil before the fragments. The uniforms stay declared in both.
uniform vec4 rgba;   // 0..1 (the darkening pass: alpha = the shader's alpha / 128)
uniform int fDate;   // TEST DATE (DATM 0): only where the frame's alpha (before the marking) is below 0x80
uniform sampler2D texDate;
out vec4 FragColor;
void main()
{
#ifndef NOKILL
    if (fDate != 0 && texelFetch(texDate, ivec2(gl_FragCoord.xy), 0).a > 254.0 / 255.0) discard;
#endif
    FragColor = rgba;
}
