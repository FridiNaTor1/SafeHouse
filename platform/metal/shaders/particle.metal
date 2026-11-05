struct ParticleVertexIn {
  float4 aClip [[attribute(0)]];
  float2 aUv [[attribute(1)]];
  float aLayer [[attribute(2)]];
  float4 aRgba [[attribute(3)]];
};

struct ParticleVaryings {
  float4 position [[position]];
  float2 vUv;
  float vLayer;
  float4 vRgba;
};

vertex ParticleVaryings vertex_main(ParticleVertexIn in [[stage_in]],
                                    constant Uniforms& u [[UNIFORMS]]) {
  ParticleVaryings out;
  out.position = CANE_FINISH_POSITION(u, in.aClip);
  out.vUv = in.aUv;
  out.vLayer = in.aLayer;
  out.vRgba = in.aRgba;
  return out;
}

fragment float4 fragment_main(ParticleVaryings in [[stage_in]],
                              constant Uniforms& u [[UNIFORMS]],
                              texture2d_array<float> tex [[texture(CANE_TEXTURE_SLOT_tex)]],
                              sampler texSampler [[sampler(CANE_TEXTURE_SLOT_tex)]]) {
  float4 c = in.vRgba;
  if (u.fTextured != 0) {
    uint layer = uint(max(round(in.vLayer), 0.0));
    c = floor(tex.sample(texSampler, in.vUv, layer) * 255.0 * in.vRgba / 128.0);
  }
  c = clamp(c, 0.0, 255.0);
  bool passes = c.a >= 128.0;
  if ((u.afailMode == 1) != passes) {
    discard_fragment();
    return float4(0.0);
  }
  return float4(c.rgb / 255.0, min(c.a / 128.0, 1.0));
}
