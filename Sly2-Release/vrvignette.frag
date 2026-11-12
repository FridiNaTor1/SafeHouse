#version 430 core
// VR (vr.cpp): the comfort vignette, darker toward the eye's edges by strength (0..1)
in vec2 vUv;
uniform vec2 center;    // the eye's optical centre in NDC
uniform vec2 scale;     // NDC to the tangent of the angle
uniform float strength;
uniform vec4 tint;      // a fill over it all (a fade, a title card's colour): rgb premultiplied by a; 0: none
out vec4 FragColor;
void main()
{
    vec2 d = (vUv * 2.0 - 1.0 - center) * scale;
    float r = length(d);
    float inner = mix(1.1, 0.35, strength);
    float a = smoothstep(inner, inner + 0.45, r) * min(strength * 1.5, 1.0);
    float A = tint.a > 0.0 ? 1.0 - (1.0 - tint.a) * (1.0 - a) : a;   // (no fill: the vignette exactly as before)
    FragColor = vec4(A > 0.0 ? tint.rgb / A : vec3(0.0), A);
}
