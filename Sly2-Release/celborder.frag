#version 430 core
uniform vec4 celRgba;   // 0..1
out vec4 FragColor;
void main() { FragColor = celRgba; }
