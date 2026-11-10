#version 430 core
// A shadow phase's DATE (the hulls' TEST DATE, DATM 0) as a stencil mark (render.cpp FMarkDate): drawn over the frame
// writing STENCIL_FBA, it keeps the pixels whose alpha before the marking (0x16DA48's copy) is 0x80 or more (1.0 here).
uniform sampler2D texDate;
out vec4 FragColor;
void main()
{
    if (texelFetch(texDate, ivec2(gl_FragCoord.xy), 0).a <= 254.0 / 255.0) discard;
    FragColor = vec4(0.0);
}
