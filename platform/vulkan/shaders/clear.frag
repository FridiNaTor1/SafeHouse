#version 430 core
// (the colour's write mask, the stencil's write mask and reference are the pipeline's and dynamic state)
uniform vec4 color;
out vec4 FragColor;
void main() { FragColor = color; }
