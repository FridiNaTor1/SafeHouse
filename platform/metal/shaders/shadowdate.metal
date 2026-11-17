// Sly2-Release shadowcombine.vert + shadowdate.frag (render.cpp FMarkDate): a shadow phase's DATE as a stencil mark,
// drawn over the frame; it keeps the pixels whose alpha before the marking (the phase's copy, texDate) is 0x80 or more.
// (Only the copy form: the Metal layer has no framebuffer fetch, render.cpp ShadowFetchMode.)
vertex RasterTarget vertex_main(uint vid [[vertex_id]], constant Uniforms& u [[UNIFORMS]]) {
  RasterTarget out;
  float2 p = float2(float((vid << 1) & 2u), float(vid & 2u));
  out.position = CANE_FINISH_POSITION(u, float4(p * 2.0 - 1.0, 0.0, 1.0));
  return out;
}

fragment float4 fragment_main(RasterTarget in [[stage_in]],
                              constant Uniforms& u [[UNIFORMS]],
                              texture2d<float> texDate [[texture(CANE_TEXTURE_SLOT_texDate)]]) {
  if (texDate.read(uint2(CANE_FRAG_COORD(u, in.position.xy))).a <= 254.0 / 255.0) discard_fragment();
  return float4(0.0);
}
