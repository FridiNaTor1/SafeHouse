#version 430 core
// The particles' GS state (0x138F08's packets): MODULATE (Ct x Cf / 128), TEST 0x7180B (ATE GEQUAL 0x80, AFAIL
// FB_ONLY, ZTST GREATER), ALPHA (Cs - Cd) x As + Cd or (Cs - 0) x As + Cd (blend state in emitter.cpp).
uniform sampler2DArray tex;
uniform int fTextured;
uniform int afailMode;   // 1: the fragments that pass (alpha >= 0x80), 2: those that fail
in vec2 vUv;
in float vLayer;
in vec4 vRgba;
out vec4 FragColor;
void main()
{
    vec4 c = vRgba;
    if (fTextured != 0) c = floor(texture(tex, vec3(vUv, vLayer)) * 255.0 * vRgba / 128.0);
    c = clamp(c, 0.0, 255.0);
    bool fPass = c.a >= 128.0;
    if ((afailMode == 1) != fPass) discard;
    FragColor = vec4(c.rgb / 255.0, min(c.a / 128.0, 1.0));
}
