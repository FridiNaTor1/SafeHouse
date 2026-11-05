#version 430 core
uniform vec4 rgba;   // 0..1 (the darkening pass: alpha = the shader's alpha / 128)
out vec4 FragColor;
void main() { FragColor = rgba; }
