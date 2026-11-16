#version 450
layout(location = 0) noperspective in vec4 vColor;
layout(location = 1) in vec2 vUv;
layout(location = 2) noperspective in float vFog;
layout(location = 0) out vec4 color;
void main() { color = vColor + vec4(vUv, vFog, 0); }
