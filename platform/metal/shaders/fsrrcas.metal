// Sly2-Release vrfull.vert + fsrrcas.frag (fsr.cpp): FidelityFX Super Resolution 1.0's RCAS (robust contrast-adaptive
// sharpening) after EASU, the 32-bit path of AMD's ffx_fsr1.h (Copyright (c) 2021 Advanced Micro Devices, Inc., MIT
// licence: licenses/fidelityfx-fsr.txt); the centre's alpha passed through. Line for line the GLSL program.
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

inline float CaneRcpMed(float a) {
  float b = as_type<float>(0x7ef19fffu - as_type<uint>(a));
  return b * (-b * a + 2.0);
}

inline float4 CaneTap(texture2d<float> tex, int2 p, int2 srcMin, int2 srcMax) {
  return tex.read(uint2(clamp(p, srcMin, srcMax)));
}

fragment float4 fragment_main(FullVaryings in [[stage_in]],
                              constant Uniforms& u [[UNIFORMS]],
                              texture2d<float> tex [[texture(CANE_TEXTURE_SLOT_tex)]]) {
  // (FSR_RCAS_LIMIT)
  const float kLimit = 0.25 - (1.0 / 16.0);
  int2 ip = int2(floor(CANE_FRAG_COORD(u, in.position.xy) - u.posOut));
  if (u.fFlipY != 0) ip.y = u.srcMax.y - u.srcMin.y - ip.y;
  int2 sp = ip + u.srcMin;
  //    b
  //  d e f
  //    h
  float3 b = CaneTap(tex, sp + int2(0, -1), u.srcMin, u.srcMax).rgb;
  float3 d = CaneTap(tex, sp + int2(-1, 0), u.srcMin, u.srcMax).rgb;
  float4 ee = CaneTap(tex, sp, u.srcMin, u.srcMax);
  float3 e = ee.rgb;
  float3 f = CaneTap(tex, sp + int2(1, 0), u.srcMin, u.srcMax).rgb;
  float3 h = CaneTap(tex, sp + int2(0, 1), u.srcMin, u.srcMax).rgb;
  float3 mn4 = min(min(min(b, d), f), h);
  float3 mx4 = max(max(max(b, d), f), h);
  float2 peakC = float2(1.0, -1.0 * 4.0);
  float3 hitMin = min(mn4, e) * (float3(1.0) / (4.0 * mx4));
  float3 hitMax = (peakC.x - max(mx4, e)) * (float3(1.0) / (4.0 * mn4 + peakC.y));
  float3 lobeRGB = max(-hitMin, hitMax);
  float lobe = max(-kLimit, min(max(max(lobeRGB.r, lobeRGB.g), lobeRGB.b), 0.0)) * u.sharpness;
  float rcpL = CaneRcpMed(4.0 * lobe + 1.0);
  return float4((lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL, ee.a);
}
