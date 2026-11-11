#version 430 core
// Upscaling (fsr.h): FidelityFX Super Resolution 1.0's EASU (edge-adaptive spatial upsampling), the 32-bit path of
// AMD's ffx_fsr1.h (Copyright (c) 2021 Advanced Micro Devices, Inc., MIT licence: licenses/fidelityfx-fsr.txt), as a
// fragment program: the 12 taps read with texelFetch (no gathers: OpenGL ES 3.0 and every layer have it), clamped to
// the source rectangle.
#ifdef CANE_ARRAY
// (VR, Vulkan multiview: the eyes' two-layer target, one layer a draw)
uniform sampler2DArray tex;
uniform int layer;
vec4 caneLoad(ivec2 p) { return texelFetch(tex, ivec3(p, layer), 0); }
#else
uniform sampler2D tex;
vec4 caneLoad(ivec2 p) { return texelFetch(tex, p, 0); }
#endif
uniform vec4 con0;      // xy: output pixel -> source pixel scale, zw: 0.5 x scale - 0.5 (FsrEasuCon con0)
uniform ivec2 srcMin;   // the source rectangle's first texel
uniform ivec2 srcMax;   // and its last
uniform vec2 posOut;    // the output rectangle's corner in the target
out vec4 FragColor;

float caneRcpLo(float a) { return uintBitsToFloat(0x7ef07ebbu - floatBitsToUint(a)); }
float caneRsqLo(float a) { return uintBitsToFloat(0x5f347d74u - (floatBitsToUint(a) >> 1u)); }
vec3 caneTap(ivec2 p) { return caneLoad(clamp(p, srcMin, srcMax)).rgb; }

void caneTapF(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c)
{
    vec2 v;
    v.x = (off.x * (dir.x)) + (off.y * dir.y);
    v.y = (off.x * (-dir.y)) + (off.y * dir.x);
    v *= len;
    float d2 = v.x * v.x + v.y * v.y;
    d2 = min(d2, clp);
    float wB = (2.0 / 5.0) * d2 + -1.0;
    float wA = lob * d2 + -1.0;
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB + -(25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

void caneSetF(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE)
{
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = caneRcpLo(lenX);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    lenX *= lenX;
    len += lenX * w;
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = caneRcpLo(lenY);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    lenY *= lenY;
    len += lenY * w;
}

float caneLuma(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }

void main()
{
    vec2 ip = floor(gl_FragCoord.xy - posOut);
    vec2 pp = ip * con0.xy + con0.zw;
    vec2 fp = floor(pp);
    pp -= fp;
    ivec2 f0 = ivec2(fp) + srcMin;
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    vec3 b = caneTap(f0 + ivec2(0, -1)), c = caneTap(f0 + ivec2(1, -1));
    vec3 e = caneTap(f0 + ivec2(-1, 0)), g = caneTap(f0 + ivec2(1, 0)), h = caneTap(f0 + ivec2(2, 0));
    vec3 i = caneTap(f0 + ivec2(-1, 1)), j = caneTap(f0 + ivec2(0, 1)), k = caneTap(f0 + ivec2(1, 1)), l = caneTap(f0 + ivec2(2, 1));
    vec3 n = caneTap(f0 + ivec2(0, 2)), o = caneTap(f0 + ivec2(1, 2));
    vec4 fA = caneLoad(clamp(f0, srcMin, srcMax));
    vec3 f = fA.rgb;
    float bL = caneLuma(b), cL = caneLuma(c), eL = caneLuma(e), fL = caneLuma(f), gL = caneLuma(g), hL = caneLuma(h);
    float iL = caneLuma(i), jL = caneLuma(j), kL = caneLuma(k), lL = caneLuma(l), nL = caneLuma(n), oL = caneLuma(o);
    vec2 dir = vec2(0.0);
    float len = 0.0;
    caneSetF(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    caneSetF(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    caneSetF(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    caneSetF(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);
    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = caneRsqLo(dirR);
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= vec2(dirR);
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) * caneRcpLo(max(abs(dir.x), abs(dir.y)));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 + -0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = caneRcpLo(lob);
    vec3 min4 = min(min(min(f, g), j), k);
    vec3 max4 = max(max(max(f, g), j), k);
    vec3 aC = vec3(0.0);
    float aW = 0.0;
    caneTapF(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    caneTapF(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    caneTapF(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    caneTapF(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    caneTapF(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    caneTapF(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    caneTapF(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    caneTapF(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    caneTapF(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    caneTapF(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    caneTapF(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
    caneTapF(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);
    // (the alpha: the nearest source texel's)
    FragColor = vec4(min(max4, max(min4, aC * vec3(1.0 / aW))), fA.a);
}
