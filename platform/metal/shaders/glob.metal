struct GlobVertexIn {
  float3 aPos [[attribute(0)]];
  float2 aUv [[attribute(1)]];
  float4 aRgbaA [[attribute(2)]];
  float4 aRgbaB [[attribute(3)]];
  float3 aNormal [[attribute(4)]];
  uint4 aBone [[attribute(5)]];
  uint aDraw [[attribute(6)]];
  float4 aWeight [[attribute(7)]];
};

struct GlobRec {
  float4x4 model;
  float4 fog;
  uint4 bone;
  float4 spec0;
  float4 spec1;
  float4 specPos;
  uint4 warp;
};

struct GlobVaryings {
  float4 position [[position]];
  float4 vColor [[center_no_perspective]];
  float2 vUv;
  float vFog [[center_no_perspective]];
};

vertex GlobVaryings vertex_main(GlobVertexIn in [[stage_in]],
                                uint vid [[vertex_id]],
                                constant Uniforms& u [[UNIFORMS]],
                                const device GlobRec* arec [[SSBO(0)]],
                                const device float4* adraw [[SSBO(1)]],
                                const device float4x4* abone [[SSBO(2)]],
                                const device float4* awarp [[SSBO(3)]],
                                const device float4x4* amatWarp [[SSBO(4)]]) {
  GlobVaryings out;
  float4 d = adraw[in.aDraw];
  GlobRec rec = arec[as_type<uint>(d.w)];
  float4 fogParams = rec.fog;
  float4 posWorld;
  if (rec.bone.y != 0u) {
    float4 p = float4(in.aPos, 1.0);
    uint last = rec.bone.y - 1u;
    posWorld = in.aWeight.x * (abone[rec.bone.x + min(in.aBone.x, last)] * p) +
               in.aWeight.y * (abone[rec.bone.x + min(in.aBone.y, last)] * p) +
               in.aWeight.z * (abone[rec.bone.x + min(in.aBone.z, last)] * p) +
               in.aWeight.w * (abone[rec.bone.x + min(in.aBone.w, last)] * p);
    posWorld.w = 1.0;
  } else {
    posWorld = rec.model * float4(in.aPos, 1.0);
  }
  float2 duv = float2(0.0);
  if (rec.warp.x != 0u) {
    uint ivtx = vid - rec.bone.z;
    uint cmat = rec.warp.w & 0xFFu;
    float3 dpos = float3(0.0);
    for (uint imat = 0u; imat < cmat; ++imat) {
      float4 st = awarp[rec.warp.x - 1u + imat * rec.warp.y + ivtx];
      dpos += (amatWarp[rec.warp.z + imat] * st).xyz;
      duv += (amatWarp[rec.warp.z + 4u + imat] * st).xy;
    }
    if ((rec.warp.w & 0x100u) != 0u) posWorld.xyz += dpos;
    if ((rec.warp.w & 0x200u) == 0u) duv = float2(0.0);
  }
  float4 position = u.matWorldToClip * posWorld;
  float4 c = u.icolor == 0 ? in.aRgbaA : (u.icolor == 1 ? in.aRgbaB : u.rgbaConst);
  float aA = in.aRgbaA.a;
  if (rec.spec1.z != 0.0 && (u.icolor == 0 || u.icolor == 3)) {
    float3x3 mInv =
        CaneInverse3x3(float3x3(rec.model[0].xyz, rec.model[1].xyz, rec.model[2].xyz));
    float3 v;
    if (rec.spec0.w == 0.0) {
      v = mInv * -u.dirCamera;
    } else {
      float3 cam = mInv * (u.posCamera - rec.model[3].xyz);
      v = rec.spec0.w == 1.0 ? cam - rec.specPos.xyz : cam - in.aPos;
    }
    v = normalize(v);
    float x = dot(v, trunc(in.aNormal * 127.0));
    float g = clamp(rec.spec0.x + rec.spec0.y * x + rec.spec0.z * x * x, rec.spec1.x,
                    rec.spec1.y);
    aA = floor(aA * g + 0.5);
    if (u.icolor == 0) c.a = aA;
  }
  if (u.icolor == 3) c.a *= aA / 128.0;
  c.a *= d.z;
  out.vColor = c;
  out.vUv = in.aUv + d.xy + duv;
  out.vFog = 0.0;
  if (fogParams.w != 0.0) {
    float x = max(position.w / fogParams.x, 1.0);
    float k = 1.0 - 1.0 / fogParams.y;
    out.vFog = k > 0.0 ? clamp(fogParams.z * (1.0 - 1.0 / x) / k, 0.0, 1.0) : 0.0;
  }
  out.position = CANE_FINISH_POSITION(u, position);
  return out;
}

inline bool GlobAlphaPasses(int atst, float a, float aref) {
  switch (atst) {
    case 0:
      return false;
    case 1:
      return true;
    case 2:
      return a < aref;
    case 3:
      return a <= aref;
    case 4:
      return a == aref;
    case 5:
      return a >= aref;
    case 6:
      return a > aref;
    default:
      return a != aref;
  }
}

fragment float4 fragment_main(GlobVaryings in [[stage_in]],
                              constant Uniforms& u [[UNIFORMS]],
                              texture2d<float> tex [[texture(CANE_TEXTURE_SLOT_tex)]],
                              sampler texSampler [[sampler(CANE_TEXTURE_SLOT_tex)]],
                              texture2d<float> texDate [[texture(CANE_TEXTURE_SLOT_texDate)]]) {
  if (u.fDate != 0 && texDate.read(uint2(in.position.xy)).a > 254.0 / 255.0) {
    discard_fragment();
    return float4(0.0);
  }
  float4 c = in.vColor;
  if (u.fTextured != 0) {
    float4 t = tex.sample(texSampler, in.vUv) * 255.0;
    c = floor(t * in.vColor / 128.0);
  }
  c = clamp(c, 0.0, 255.0);
  if (u.afailMode != 0 && u.ate != 0) {
    bool passes = GlobAlphaPasses(u.atst, c.a, u.aref);
    if ((u.afailMode == 1) != passes) {
      discard_fragment();
      return float4(0.0);
    }
  }
  float3 rgb = c.rgb / 255.0;
  rgb = mix(rgb, u.fFogBlack != 0 ? float3(0.0) : u.fogColor.rgb, in.vFog);
  return float4(rgb, min(c.a / 128.0, 1.0));
}
