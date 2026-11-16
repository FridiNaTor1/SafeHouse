#version 430 core
// VR (vr.cpp): the RC table's aim mark, already in clip space (both eyes in one pass on Vulkan: in world space, each
// eye's matrix from CaneViews)
layout (location = 0) in vec4 aClip;
#ifdef CANE_MULTIVIEW
void main() { gl_Position = caneViewMat[gl_ViewIndex] * aClip; }
#else
void main() { gl_Position = aClip; }
#endif
