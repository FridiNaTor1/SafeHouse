#version 430 core
uniform sampler2D tex;
in vec2 vUv;
out vec4 FragColor;
void main() { FragColor = vec4(texture(tex, vUv).rgb, 1.0); }
