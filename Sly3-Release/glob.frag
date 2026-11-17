#version 430 core
// Sly 2 glob fragments: the GS's texture function (MODULATE, TCC 1) and alpha test, for one pass of a shader.
// The pass's ALPHA blend and depth test are GL state (render.cpp).
// NOKILL: the variant without the alpha test and DATE (no discard), for the draws that use neither, so the GPU tests
// depth and stencil before the fragments (render.cpp picks the program per run). The uniforms stay declared in both.

uniform sampler2D tex;
uniform int  fTextured;
uniform int  ate;            // TEST: alpha test enable
uniform int  atst;           //       0 never 1 always 2 less 3 lequal 4 equal 5 gequal 6 greater 7 notequal
uniform float aref;          //       reference (GS units)
uniform int  afailMode;      // 0: no test, 1: keep fragments that pass, 2: keep fragments that fail
uniform vec4 fogColor;       // 0..1
uniform int  fFogBlack;      // the pass's FOGCOL is 0 (0x1EB960: kinds 1 / 7's additive passes), not fogColor
uniform int  fDate;          // TEST DATE, DATM 0 (the shadow hulls): only where the frame's alpha before the marking
uniform sampler2D texDate;   //   (texDate, 0x16DA48's copy) is below 0x80

#ifdef BINDLESS
flat in uvec2 vTexh;
#endif
#if defined(CANE_BLEND1) && defined(BINDLESS)
// (glob.vert CANE_DRAWSTATE: the pass draw's state word; bit 31 clear: the uniforms)
#define CANE_DRAWSTATE 1
flat in uint vDrawState;
#endif
#ifdef TEXARRAY
uniform sampler2DArray texArr;   // (perf-5: the run's page of layers, unit 2; vLayer 0: the bound tex)
flat in uint vLayer;
#endif
noperspective in vec4 vColor;
in vec2 vUv;
noperspective in float vFog;

out vec4 FragColor;
#ifdef CANE_BLEND1
// (the Vulkan renderer on Android, render.cpp g_fBlend1: the GS's two common blends, (Cs - Cd) x As + Cd and Cs x As + Cd,
// as one fixed blend, ONE / ONE_MINUS_SRC1_ALPHA: the colour premultiplied here, the second output's alpha As or 0.
// 0: the blend is GL state as usual. (Without dynamic blend state each blend change is a pipeline bind.))
uniform int caneBlend1;
layout(location = 0, index = 1) out vec4 FragColor1;
#endif

bool AlphaPasses(float a)
{
    switch (atst)
    {
    case 0: return false;
    case 1: return true;
    case 2: return a < aref;
    case 3: return a <= aref;
    case 4: return a == aref;
    case 5: return a >= aref;
    case 6: return a > aref;
    default: return a != aref;
    }
}

void main()
{
#ifdef CANE_DRAWSTATE
    bool fDs = (vDrawState & 0x80000000u) != 0u;
    int fFogBlackDraw = fDs ? int((vDrawState >> 2) & 1u) : fFogBlack;
    int caneBlend1Draw = fDs ? int((vDrawState >> 3) & 3u) : caneBlend1;
#define fFogBlack fFogBlackDraw
#define caneBlend1 caneBlend1Draw
#endif
#ifndef NOKILL
    if (fDate != 0 && texelFetch(texDate, ivec2(gl_FragCoord.xy), 0).a > 254.0 / 255.0) discard;
#endif
    vec4 c = vColor;
    if (fTextured != 0)
    {
#ifdef BINDLESS
        // (the same texture and sampler as a bound one: handle 0 is the special draws', which bind tex)
        vec4 t = (vTexh != uvec2(0u) ? texture(sampler2D(vTexh), vUv) : texture(tex, vUv)) * 255.0;
#elif defined(TEXARRAY)
        // (the same texels and sampler as the bound 2D texture: the layer is its copy)
        vec4 t = (vLayer != 0u ? texture(texArr, vec3(vUv, float(vLayer - 1u))) : texture(tex, vUv)) * 255.0;
#else
        vec4 t = texture(tex, vUv) * 255.0;
#endif
#if defined(CANE_FP16) && CANE_FP16 >= 2
        // (throwaway experiment, SLY2N_VKFP16=2: the texture function's product in float16 too; not exact)
        c = vec4(floor(f16vec4(t) * f16vec4(vColor) / float16_t(128.0)));
#else
        c = floor(t * vColor / 128.0);
#endif
    }
    c = clamp(c, 0.0, 255.0);
#ifndef NOKILL
    if (afailMode != 0 && ate != 0)
    {
        bool fPass = AlphaPasses(c.a);
        if ((afailMode == 1) != fPass) discard;
    }
#endif
#ifdef CANE_FP16
    // (throwaway experiment, SLY2N_VKFP16=1: the fog mix and the alpha in float16; the fog mix is not exact)
    f16vec3 rgbh = f16vec3(c.rgb) / float16_t(255.0);
    rgbh = mix(rgbh, fFogBlack != 0 ? f16vec3(0.0) : f16vec3(fogColor.rgb), float16_t(vFog));
    FragColor = vec4(vec3(rgbh), float(min(float16_t(c.a) / float16_t(128.0), float16_t(1.0))));
#else
    vec3 rgb = c.rgb / 255.0;
    rgb = mix(rgb, fFogBlack != 0 ? vec3(0.0) : fogColor.rgb, vFog);
    // The GS blends with As / 128 (0x80 = 1.0).
    FragColor = vec4(rgb, min(c.a / 128.0, 1.0));
#endif
#ifdef CANE_BLEND1
    if (caneBlend1 != 0) FragColor.rgb *= FragColor.a;
    FragColor1 = vec4(0.0, 0.0, 0.0, caneBlend1 == 1 ? FragColor.a : 0.0);
#endif
}
