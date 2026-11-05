vertex RasterTarget vertex_main(uint vid [[vertex_id]], constant Uniforms& u [[UNIFORMS]]) {
  RasterTarget out;
  float2 p = float2(float((vid << 1) & 2u), float(vid & 2u));
  out.position = CANE_FINISH_POSITION(u, float4(p * 2.0 - 1.0, 0.0, 1.0));
  return out;
}

fragment float4 fragment_main(RasterTarget in [[stage_in]],
                              constant Uniforms& u [[UNIFORMS]],
                              texture2d<float> texAlpha [[texture(CANE_TEXTURE_SLOT_texAlpha)]]) {
  if (u.fReset != 0) return float4(0.0);
  int a = int(round(texAlpha.read(uint2(in.position.xy)).a * 128.0));
  int level = max((a & 0x7E) + u.m8 - 1, 0);
  float p = floor(float(level) * 0.007936508 * (u.gain * 128.0));
  return float4(0.0, 0.0, 0.0, p / 128.0);
}
