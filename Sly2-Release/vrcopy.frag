#version 430 core
// VR (vr.cpp): the game picture onto the headset's panel image (opaque for the Flat Screen; with its alpha for the HUD)
in vec2 vUv;
#ifdef CANE_ARRAY
// (Vulkan VR multiview: the eyes' two-layer target into the image's two layers in one two-view draw, each view its layer)
uniform sampler2DArray tex;
uniform int layer;
#else
uniform sampler2D tex;
#endif
uniform int fOpaque;
// (1 on the Vulkan renderer: its images keep GL's rows, bottom first, and an OpenXR Vulkan image is read top first)
uniform int fFlipY;
uniform vec2 uvScale;   // (the eye resolution: the part of the source drawn)
out vec4 FragColor;
void main()
{
#ifdef CANE_ARRAY
    vec4 c = texture(tex, vec3((fFlipY != 0 ? vec2(vUv.x, 1.0 - vUv.y) : vUv) * uvScale, float(layer)));
#else
    vec4 c = texture(tex, (fFlipY != 0 ? vec2(vUv.x, 1.0 - vUv.y) : vUv) * uvScale);
#endif
    FragColor = fOpaque != 0 ? vec4(c.rgb, 1.0) : c;
}
