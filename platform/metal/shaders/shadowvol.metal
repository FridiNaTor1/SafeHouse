struct ShadowVolVertexIn {
  float3 aPos [[attribute(0)]];
};

vertex RasterTarget vertex_main(ShadowVolVertexIn in [[stage_in]],
                                constant Uniforms& u [[UNIFORMS]]) {
  RasterTarget out;
  out.position = CANE_FINISH_POSITION(u, u.matWorldToClip * float4(in.aPos, 1.0));
  return out;
}

fragment float4 fragment_main(RasterTarget in [[stage_in]],
                              constant Uniforms& u [[UNIFORMS]],
                              texture2d<float> texDate [[texture(CANE_TEXTURE_SLOT_texDate)]]) {
  if (u.fDate != 0 && texDate.read(uint2(in.position.xy)).a > 254.0 / 255.0) {
    discard_fragment();
    return float4(0.0);
  }
  return u.rgba;
}
