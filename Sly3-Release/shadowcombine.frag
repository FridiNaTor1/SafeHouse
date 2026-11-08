#version 430 core
// 0x16DD58's last pass: the frame tinted through a 256-entry CLUT (0x1EA9F0) indexed by the frame's alpha byte. The
// index is the scene's alpha (bits 1..6, kept through the shuffles) with the kind-7 mark in bit 7 and the kind-8 mark in
// bit 0; index & 0x81 picks the ramp (GAME +0x3E64's gains 128 / 96 / 160 for 0x80 / 0x01 / 0x81), the level is
// (index & 0x7F) - 1 over 126, the colour black (+0x3E70); blended (Cs - Cd) x As / 128 + Cd (ALPHA 0x44), and the
// CLUT's alpha is what the frame keeps. render.cpp draws one ramp per stencil class (render.md "shadow frame-channel").
uniform sampler2D texAlpha;  // the frame before the marking (0x16DA48's copy): alpha / 128 as the native frame keeps it
uniform float gain;          // the ramp's gain / 255
uniform int m8;              // the kind-8 mark (the index's bit 0)
uniform int fReset;          // the pixels no hull marked: the CLUT's entries with index & 0x81 == 0 are 0
out vec4 FragColor;
void main()
{
    if (fReset != 0) { FragColor = vec4(0.0); return; }
    int a = int(round(texelFetch(texAlpha, ivec2(gl_FragCoord.xy), 0).a * 128.0));   // the GS alpha (128: >= 0x80)
    int level = max((a & 0x7E) + m8 - 1, 0);
    // SetClr: (int)(g x gA x 128), g = level x (1 / 126), gA = gain x (1 / 255)
    float p = floor(float(level) * 0.007936508 * (gain * 128.0));
    FragColor = vec4(0.0, 0.0, 0.0, p / 128.0);
}
