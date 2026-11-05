#version 430 core
// The cast shadows' volumes (mesh type 2, built in world space by render.cpp from the light context's shadow).
layout (location = 0) in vec3 aPos;
uniform mat4 matWorldToClip;
void main() { gl_Position = matWorldToClip * vec4(aPos, 1.0); }
