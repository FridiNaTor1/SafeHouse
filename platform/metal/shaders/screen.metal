struct ScreenVaryings {
  float4 position [[position]];
  float2 vUv;
};

vertex ScreenVaryings vertex_main(uint vid [[vertex_id]], constant Uniforms& u [[UNIFORMS]]) {
  const float2 corners[6] = {float2(0, 0), float2(1, 0), float2(0, 1),
                             float2(0, 1), float2(1, 0), float2(1, 1)};
  ScreenVaryings out;
  float2 p = corners[vid % 6u];
  out.vUv = float2(p.x, 1.0 - p.y);
  out.position = CANE_FINISH_POSITION(u, float4(p * 2.0 - 1.0, 0.0, 1.0));
  return out;
}

fragment float4 fragment_main(ScreenVaryings in [[stage_in]],
                              texture2d<float> tex [[texture(CANE_TEXTURE_SLOT_tex)]],
                              sampler texSampler [[sampler(CANE_TEXTURE_SLOT_tex)]]) {
  return float4(tex.sample(texSampler, in.vUv).rgb, 1.0);
}
