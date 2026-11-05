#version 430 core
// orig0x1477D8: already-quantized512x224 GS pixels; STQ interpolates linearly.
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec3 aStq;
uniform vec4 uCanvas;
noperspective out vec3 vStq;
void main()
{
    vec2 ndc = vec2(aPos.x / 256.0 - 1.0, 1.0 - aPos.y / 112.0);
    gl_Position = vec4(ndc * uCanvas.xy + uCanvas.zw, 0.0, 1.0);
    vStq = aStq;
}
