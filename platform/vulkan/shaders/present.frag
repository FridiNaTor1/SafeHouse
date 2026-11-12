#version 430 core
uniform sampler2D tex;
in vec2 uv;
out vec4 FragColor;
void main() { FragColor = vec4(texture(tex, uv).rgb, 1.0); }
