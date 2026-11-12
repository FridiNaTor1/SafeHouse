#version 430 core
// Upscaling (fsr.h): FidelityFX Super Resolution 1.0's RCAS (robust contrast-adaptive sharpening) after EASU, the
// 32-bit path of AMD's ffx_fsr1.h (Copyright (c) 2021 Advanced Micro Devices, Inc., MIT licence:
// licenses/fidelityfx-fsr.txt), as a fragment program; the centre's alpha passed through.
uniform sampler2D tex;
uniform float sharpness;   // linear: 2^-stops (FsrRcasCon con.x), 1 the most
uniform ivec2 srcMin;   // the source rectangle's first texel
uniform ivec2 srcMax;   // and its last
uniform vec2 posOut;       // the output rectangle's corner in the target
uniform int fFlipY;        // (VR on Vulkan: the rows the other way up, as vrcopy.frag: an OpenXR image is read top first)
out vec4 FragColor;

float caneRcpMed(float a) { float b = uintBitsToFloat(0x7ef19fffu - floatBitsToUint(a)); return b * (-b * a + 2.0); }
vec4 caneTap(ivec2 p) { return texelFetch(tex, clamp(p, srcMin, srcMax), 0); }

void main()
{
    // (FSR_RCAS_LIMIT)
    const float kLimit = 0.25 - (1.0 / 16.0);
    ivec2 ip = ivec2(floor(gl_FragCoord.xy - posOut));
    if (fFlipY != 0) ip.y = srcMax.y - srcMin.y - ip.y;
    ivec2 sp = ip + srcMin;
    //    b
    //  d e f
    //    h
    vec3 b = caneTap(sp + ivec2(0, -1)).rgb;
    vec3 d = caneTap(sp + ivec2(-1, 0)).rgb;
    vec4 ee = caneTap(sp);
    vec3 e = ee.rgb;
    vec3 f = caneTap(sp + ivec2(1, 0)).rgb;
    vec3 h = caneTap(sp + ivec2(0, 1)).rgb;
    vec3 mn4 = min(min(min(b, d), f), h);
    vec3 mx4 = max(max(max(b, d), f), h);
    vec2 peakC = vec2(1.0, -1.0 * 4.0);
    vec3 hitMin = min(mn4, e) * (vec3(1.0) / (4.0 * mx4));
    vec3 hitMax = (peakC.x - max(mx4, e)) * (vec3(1.0) / (4.0 * mn4 + peakC.y));
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-kLimit, min(max(max(lobeRGB.r, lobeRGB.g), lobeRGB.b), 0.0)) * sharpness;
    float rcpL = caneRcpMed(4.0 * lobe + 1.0);
    FragColor = vec4((lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL, ee.a);
}
