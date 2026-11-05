struct ClearParams {
  float4 color;
  float depth;
};

vertex RasterTarget clear_vertex(uint vid [[vertex_id]], constant ClearParams& p [[UNIFORMS]]) {
  RasterTarget out;
  float2 xy = float2(float((vid << 1) & 2u), float(vid & 2u)) * 2.0 - 1.0;
  out.position = float4(xy, p.depth, 1.0);
  return out;
}

fragment float4 clear_fragment(constant ClearParams& p [[UNIFORMS]]) { return p.color; }

struct BlitParams {
  float4 dst_ndc;
  float4 src_uv;
};

struct BlitVaryings {
  float4 position [[position]];
  float2 uv;
};

vertex BlitVaryings blit_vertex(uint vid [[vertex_id]], constant BlitParams& p [[UNIFORMS]]) {
  BlitVaryings out;
  float2 t = float2(float(vid & 1u), float((vid >> 1) & 1u));
  out.position = float4(mix(p.dst_ndc.xy, p.dst_ndc.zw, t), 0.0, 1.0);
  out.uv = mix(p.src_uv.xy, p.src_uv.zw, t);
  return out;
}

fragment float4 blit_fragment(BlitVaryings in [[stage_in]],
                              texture2d<float> src [[texture(0)]],
                              sampler s [[sampler(0)]]) {
  return src.sample(s, in.uv);
}
