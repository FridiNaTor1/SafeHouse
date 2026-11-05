struct HudVertexIn {
  float2 aPos [[attribute(0)]];
  float4 aRgba [[attribute(1)]];
  float aZ [[attribute(2)]];
  float2 aUv [[attribute(3)]];
};

struct HudVaryings {
  float4 position [[position]];
  float4 vColor;
  float2 vUv;
};

vertex HudVaryings vertex_main(HudVertexIn in [[stage_in]], constant Uniforms& u [[UNIFORMS]]) {
  HudVaryings out;
  float2 ndc = float2(in.aPos.x / 320.0 - 1.0, 1.0 - in.aPos.y / 224.0);
  float4 position = float4(ndc * u.uCanvas.xy + u.uCanvas.zw, in.aZ, 1.0);
  out.position = CANE_FINISH_POSITION(u, position);
  out.vColor = in.aRgba;
  out.vUv = in.aUv;
  return out;
}

fragment float4 fragment_main(HudVaryings in [[stage_in]],
                              constant Uniforms& u [[UNIFORMS]],
                              texture2d<float> tex [[texture(CANE_TEXTURE_SLOT_tex)]],
                              sampler texSampler [[sampler(CANE_TEXTURE_SLOT_tex)]]) {
  float4 c = in.vColor;
  if (u.fTextured != 0) c = floor(tex.sample(texSampler, in.vUv) * 255.0 * in.vColor / 128.0);
  if (u.fAlphaNonZero != 0 && c.a <= 0.0) {
    discard_fragment();
    return float4(0.0);
  }
  return float4(clamp(c.rgb, 0.0, 255.0) / 255.0, clamp(c.a / 128.0, 0.0, 1.0));
}
