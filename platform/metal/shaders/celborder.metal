struct CelEdgeSkin {
  float4 w;
  uint4 b;
};

inline float4 CelPosWorld(int i, constant Uniforms& u, const device float4* edges,
                          const device CelEdgeSkin* askin, const device float4x4* abone) {
  float4 p = float4(edges[i].xyz, 1.0);
  if (u.cbone <= 0) return u.matModel * p;
  CelEdgeSkin s = askin[i];
  uint c = uint(u.cbone) - 1u;
  uint f = uint(u.iboneFirst);
  float4 q = s.w.x * (abone[f + min(s.b.x, c)] * p) + s.w.y * (abone[f + min(s.b.y, c)] * p) +
             s.w.z * (abone[f + min(s.b.z, c)] * p) + s.w.w * (abone[f + min(s.b.w, c)] * p);
  return float4(q.xyz, 1.0);
}

inline float CelCross2D(float2 a, float2 b) { return a.x * b.y - a.y * b.x; }

vertex RasterTarget vertex_main(uint vid [[vertex_id]], constant Uniforms& u [[UNIFORMS]],
                                const device float4* edges [[SSBO(8)]],
                                const device CelEdgeSkin* askin [[SSBO(9)]],
                                const device float4x4* abone [[SSBO(2)]]) {
  RasterTarget out;
  const float4 culled = float4(2.0, 2.0, 2.0, 1.0);
  int iedge = u.edgeBase + int(vid) / 6;
  int icorner = int(vid) % 6;
  float4 A = u.matWorldToClip * CelPosWorld(iedge * 4 + 0, u, edges, askin, abone);
  float4 B = u.matWorldToClip * CelPosWorld(iedge * 4 + 1, u, edges, askin, abone);
  float4 C = u.matWorldToClip * CelPosWorld(iedge * 4 + 2, u, edges, askin, abone);
  float4 D = u.matWorldToClip * CelPosWorld(iedge * 4 + 3, u, edges, askin, abone);
  if (A.w <= 0.0 || B.w <= 0.0 || C.w <= 0.0 || D.w <= 0.0) {
    out.position = culled;
    return out;
  }
  float2 a = A.xy / A.w;
  float2 b = B.xy / B.w;
  float2 c = C.xy / C.w;
  float2 d = D.xy / D.w;
  float2 d2 = b - a;
  float facingA = CelCross2D(a - c, d2);
  float facingB = CelCross2D(d - b, d2);
  if (!(facingA * facingB < 0.0)) {
    out.position = culled;
    return out;
  }
  float2 dirPx = d2 / u.halfWidthNdc;
  float len = length(dirPx);
  if (len < 1e-6) {
    out.position = culled;
    return out;
  }
  float2 unit = dirPx / len;
  float side = facingA < 0.0 ? -1.0 : 1.0;
  float2 perp = float2(unit.y, -unit.x) * side;
  bool fB = icorner == 2 || icorner == 4 || icorner == 5;
  bool fOut = icorner == 1 || icorner == 2 || icorner == 4;
  float4 P = fB ? B : A;
  float2 off = (fOut ? perp * 2.0 : float2(0.0)) + (fB ? unit : -unit) * 0.5;
  P.xy += off * u.halfWidthNdc * P.w;
  out.position = CANE_FINISH_POSITION(u, P);
  return out;
}

fragment float4 fragment_main(constant Uniforms& u [[UNIFORMS]]) { return u.celRgba; }
