#version 430 core
// 2D (P_K21's role): PS2 frame pixels (640 x 448, y down) to NDC; colours in GS units.
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec4 aRgba;
layout (location = 2) in float aZ;
layout (location = 3) in vec2 aUv;
uniform vec4 uCanvas;   // the canvas in the picture (display.h): NDC scale xy, offset zw
out vec4 vColor;
out vec2 vUv;
void main()
{
    vec2 ndc = vec2(aPos.x / 320.0 - 1.0, 1.0 - aPos.y / 224.0);
    gl_Position = vec4(ndc * uCanvas.xy + uCanvas.zw, aZ, 1.0);
    vColor = aRgba;
    vUv = aUv;
}
