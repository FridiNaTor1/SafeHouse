#version 430 core
// The GL layer's window picture drawn into a pre-rotated swapchain image (gl_vulkan.cpp RecordPresentDraw): one
// triangle over the image. The swapchain's transform (VK_SURFACE_TRANSFORM_ROTATE_90 / 180 / 270: `rotation` 1..3)
// is the turn the picture needs in the image for the display to show it upright, so each image point samples the
// picture there; the picture is in GL's row order (row 0 at the bottom). (Checked on the emulator's rotated display.)
uniform int rotation;
out vec2 uv;
void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    vec2 s = p;
    if (rotation == 1) s = vec2(p.y, 1.0 - p.x);
    else if (rotation == 2) s = vec2(1.0 - p.x, 1.0 - p.y);
    else if (rotation == 3) s = vec2(1.0 - p.y, p.x);
    uv = vec2(s.x, 1.0 - s.y);
}
