#version 430 core

flat in vec4  vCelRgba;
flat in float vAlphaCelBorder;

out vec4 FragColor;

void main()
{
	FragColor.rgb = vCelRgba.rgb;
	FragColor.a = clamp(vCelRgba.a * vAlphaCelBorder, 0.0, 1.0);
}