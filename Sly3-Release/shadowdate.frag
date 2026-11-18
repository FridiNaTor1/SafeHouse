#version 430 core
// A shadow phase's DATE (the hulls' TEST DATE, DATM 0) as a stencil mark (render.cpp FMarkDate): drawn over the frame
// writing STENCIL_FBA, it keeps the pixels whose alpha before the marking (0x16DA48's copy) is 0x80 or more (1.0 here).
// CANE_FETCH: the frame's own alpha read in place (shadowcombine.frag).
uniform sampler2D texDate;
#if defined(CANE_FETCH) && defined(VULKAN)
out vec4 FragColor;   // (the Vulkan layer: the attachment as input attachment caneFbFetch, cmake/vulkan_shaders.py)
#define CANE_FB_ALPHA subpassLoad(caneFbFetch).a
#elif defined(CANE_FETCH_NC)
layout(noncoherent) inout vec4 FragColor;
#define CANE_FB_ALPHA FragColor.a
#elif defined(CANE_FETCH)
inout vec4 FragColor;
#define CANE_FB_ALPHA FragColor.a
#else
out vec4 FragColor;
#endif
void main()
{
#ifdef CANE_FETCH
    if (CANE_FB_ALPHA <= 254.0 / 255.0) discard;
#else
    if (texelFetch(texDate, ivec2(gl_FragCoord.xy), 0).a <= 254.0 / 255.0) discard;
#endif
    FragColor = vec4(0.0);
}
