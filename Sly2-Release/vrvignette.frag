#version 430 core
// VR (vr.cpp): the comfort vignette, darker toward the eye's edges by strength (0..1)
in vec2 vUv;
uniform vec2 center;    // the eye's optical centre in NDC
uniform vec2 scale;     // NDC to the tangent of the angle
uniform float strength;
out vec4 FragColor;
void main()
{
    vec2 d = (vUv * 2.0 - 1.0 - center) * scale;
    float r = length(d);
    float inner = mix(1.1, 0.35, strength);
    float a = smoothstep(inner, inner + 0.45, r) * min(strength * 1.5, 1.0);
    FragColor = vec4(0.0, 0.0, 0.0, a);
}
