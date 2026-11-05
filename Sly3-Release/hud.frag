#version 430 core
in vec4 vColor;
in vec2 vUv;
uniform sampler2D tex;
uniform int fTextured;
uniform int fAlphaNonZero;   // TEST ATE, ATST NOTEQUAL, AREF 0 (kind-6 quads: a render target's drawn pixels)
out vec4 FragColor;
// The GS blends with As / 128 (ALPHA 0x44: (Cs - Cd) x As + Cd).
void main()
{
    vec4 c = vColor;
    // MODULATE: Ct x Cf / 128 (GS units)
    if (fTextured != 0) c = floor(texture(tex, vUv) * 255.0 * vColor / 128.0);
    if (fAlphaNonZero != 0 && c.a <= 0.0) discard;
    FragColor = vec4(clamp(c.rgb, 0.0, 255.0) / 255.0, clamp(c.a / 128.0, 0.0, 1.0));
}
