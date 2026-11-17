// Sly2-Release vrfull.vert + fsreasu.frag (fsr.cpp): FidelityFX Super Resolution 1.0's EASU (edge-adaptive spatial
// upsampling), the 32-bit path of AMD's ffx_fsr1.h (Copyright (c) 2021 Advanced Micro Devices, Inc., MIT licence:
// licenses/fidelityfx-fsr.txt): the 12 taps read directly, clamped to the source rectangle. Line for line the GLSL
// program. CANE_ARRAY (shader_library.mm's second entry): the source a layer of a 2D array (VR multiview).
struct FullVaryings {
  float4 position [[position]];
  float2 vUv;
};

vertex FullVaryings vertex_main(uint vid [[vertex_id]], constant Uniforms& u [[UNIFORMS]]) {
  FullVaryings out;
  float2 p = float2(float((vid << 1) & 2u), float(vid & 2u));
  out.vUv = p;
  out.position = CANE_FINISH_POSITION(u, float4(p * 2.0 - 1.0, 0.0, 1.0));
  return out;
}

#ifdef CANE_ARRAY
typedef texture2d_array<float> CaneSource;
#define CANE_LOAD(p) tex.read(uint2(p), uint(u.layer))
#else
typedef texture2d<float> CaneSource;
#define CANE_LOAD(p) tex.read(uint2(p))
#endif

inline float CaneRcpLo(float a) { return as_type<float>(0x7ef07ebbu - as_type<uint>(a)); }
inline float CaneRsqLo(float a) { return as_type<float>(0x5f347d74u - (as_type<uint>(a) >> 1u)); }

inline void CaneTapF(thread float3& aC, thread float& aW, float2 off, float2 dir, float2 len, float lob, float clp,
                     float3 c) {
  float2 v;
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

inline void CaneSetF(thread float2& dir, thread float& len, float w, float lA, float lB, float lC, float lD, float lE) {
  float dc = lD - lC;
  float cb = lC - lB;
  float lenX = max(abs(dc), abs(cb));
  lenX = CaneRcpLo(lenX);
  float dirX = lD - lB;
  dir.x += dirX * w;
  lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
  lenX *= lenX;
  len += lenX * w;
  float ec = lE - lC;
  float ca = lC - lA;
  float lenY = max(abs(ec), abs(ca));
  lenY = CaneRcpLo(lenY);
  float dirY = lE - lA;
  dir.y += dirY * w;
  lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
  lenY *= lenY;
  len += lenY * w;
}

inline float CaneLuma(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }

fragment float4 fragment_main(FullVaryings in [[stage_in]],
                              constant Uniforms& u [[UNIFORMS]],
                              CaneSource tex [[texture(CANE_TEXTURE_SLOT_tex)]]) {
  const int2 srcMin = u.srcMin, srcMax = u.srcMax;
#define CANE_TAP(o) CANE_LOAD(clamp(f0 + (o), srcMin, srcMax)).rgb
  float2 ip = floor(CANE_FRAG_COORD(u, in.position.xy) - u.posOut);
  float2 pp = ip * u.con0.xy + u.con0.zw;
  float2 fp = floor(pp);
  pp -= fp;
  int2 f0 = int2(fp) + srcMin;
  //    b c
  //  e f g h
  //  i j k l
  //    n o
  float3 b = CANE_TAP(int2(0, -1)), c = CANE_TAP(int2(1, -1));
  float3 e = CANE_TAP(int2(-1, 0)), g = CANE_TAP(int2(1, 0)), h = CANE_TAP(int2(2, 0));
  float3 i = CANE_TAP(int2(-1, 1)), j = CANE_TAP(int2(0, 1)), k = CANE_TAP(int2(1, 1)), l = CANE_TAP(int2(2, 1));
  float3 n = CANE_TAP(int2(0, 2)), o = CANE_TAP(int2(1, 2));
#undef CANE_TAP
  float4 fA = CANE_LOAD(clamp(f0, srcMin, srcMax));
  float3 f = fA.rgb;
  float bL = CaneLuma(b), cL = CaneLuma(c), eL = CaneLuma(e), fL = CaneLuma(f), gL = CaneLuma(g), hL = CaneLuma(h);
  float iL = CaneLuma(i), jL = CaneLuma(j), kL = CaneLuma(k), lL = CaneLuma(l), nL = CaneLuma(n), oL = CaneLuma(o);
  float2 dir = float2(0.0);
  float len = 0.0;
  CaneSetF(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
  CaneSetF(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
  CaneSetF(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
  CaneSetF(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);
  float2 dir2 = dir * dir;
  float dirR = dir2.x + dir2.y;
  bool zro = dirR < (1.0 / 32768.0);
  dirR = CaneRsqLo(dirR);
  dirR = zro ? 1.0 : dirR;
  dir.x = zro ? 1.0 : dir.x;
  dir *= float2(dirR);
  len = len * 0.5;
  len *= len;
  float stretch = (dir.x * dir.x + dir.y * dir.y) * CaneRcpLo(max(abs(dir.x), abs(dir.y)));
  float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 + -0.5 * len);
  float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
  float clp = CaneRcpLo(lob);
  float3 min4 = min(min(min(f, g), j), k);
  float3 max4 = max(max(max(f, g), j), k);
  float3 aC = float3(0.0);
  float aW = 0.0;
  CaneTapF(aC, aW, float2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
  CaneTapF(aC, aW, float2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
  CaneTapF(aC, aW, float2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
  CaneTapF(aC, aW, float2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
  CaneTapF(aC, aW, float2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
  CaneTapF(aC, aW, float2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
  CaneTapF(aC, aW, float2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
  CaneTapF(aC, aW, float2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
  CaneTapF(aC, aW, float2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
  CaneTapF(aC, aW, float2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
  CaneTapF(aC, aW, float2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
  CaneTapF(aC, aW, float2(0.0, 2.0) - pp, dir, len2, lob, clp, n);
  // (the alpha: the nearest source texel's)
  return float4(min(max4, max(min4, aC * float3(1.0 / aW))), fA.a);
}
