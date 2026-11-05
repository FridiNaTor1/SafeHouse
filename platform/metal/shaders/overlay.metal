struct OverlayVertexIn {
  float2 aPos [[attribute(0)]];
  float3 aStq [[attribute(1)]];
};

struct OverlayVaryings {
  float4 position [[position]];
  float3 vStq [[center_no_perspective]];
};

vertex OverlayVaryings vertex_main(OverlayVertexIn in [[stage_in]],
                                   constant Uniforms& u [[UNIFORMS]]) {
  OverlayVaryings out;
  float2 ndc = float2(in.aPos.x / 256.0 - 1.0, 1.0 - in.aPos.y / 112.0);
  float4 position = float4(ndc * u.uCanvas.xy + u.uCanvas.zw, 0.0, 1.0);
  out.position = CANE_FINISH_POSITION(u, position);
  out.vStq = in.aStq;
  return out;
}

fragment float4 fragment_main(OverlayVaryings in [[stage_in]],
                              texture2d<float> tex [[texture(CANE_TEXTURE_SLOT_tex)]],
                              sampler texSampler [[sampler(CANE_TEXTURE_SLOT_tex)]]) {
  float2 uv = in.vStq.xy / in.vStq.z;
  float2 atlas = float2(uv.x * 2.0, 1.0 - uv.y * (1024.0 / 224.0));
  return float4(tex.sample(texSampler, atlas).rgb, 1.0);
}
