#version 430 core
// Sly 2 glob program (type-0 meshes), adapted from Sly1-Release glob.vert: what the VU1 program P_G computes for
// a vertex (transform, the pass's vertex colour) done the GL way. Colours stay in GS units (0..255, 0x80 = 1.0).

#ifdef DEKO3D
// (the Switch: the position computed exactly as written, so celborder.vert's depth test (GEQUAL against this depth)
// sees the same bits: the compiler would otherwise fuse multiply-adds differently in the two programs)
invariant gl_Position;
precise gl_Position;
#endif

// (with glob.geom, the near-plane clip: the outputs go to it under these names, and it passes them on as v*)
#ifdef GEOM
#define vColor gsColor
#define vUv gsUv
#define vFog gsFog
#define vTexh gsTexh
#define vLayer gsLayer
#endif

#ifdef CANE_PULL
// (OpenGL ES without EXT_base_instance, gl.cpp g_fDrawUniform: render.cpp draws a run of sub-draws as one draw of a
// per-frame index list; each index holds the run's sub-draw (bits 22..31, from caneDrawBase) and the vertex (bits
// 0..21, from caneVertexBase), and the vertex is read here from the arena's buffers (render.cpp GPUVTX / GPUCOL))
layout (std430, binding = 6) readonly buffer PULLVTX { uint apullv[]; };   // GPUVTX: 12 words a vertex
layout (std430, binding = 7) readonly buffer PULLCOL { uint apullc[]; };   // GPUCOL: 2 words a vertex
layout (std430, binding = 10) readonly buffer PULLDRAWMAP { uint adrawmap[]; };   // the pass's segments' draw indices
uniform uint caneDrawBase;
uniform uint caneVertexBase;
vec3 aPos;
vec2 aUv;
vec4 aRgbaA;
vec4 aRgbaB;
vec3 aNormal;
uvec4 aBone;
uint aDraw;
vec4 aWeight;
uint caneVertexId;
uvec4 CaneBytes(uint w) { return uvec4(w & 255u, (w >> 8) & 255u, (w >> 16) & 255u, w >> 24); }
void CanePull()
{
    uint id = uint(gl_VertexID);
    caneVertexId = caneVertexBase + (id & 0x3FFFFFu);
    aDraw = (caneDrawBase & 0x80000000u) != 0u ? (caneDrawBase & 0x7FFFFFFFu) : adrawmap[caneDrawBase + (id >> 22)];
    uint o = caneVertexId * 12u;
    aPos = vec3(uintBitsToFloat(apullv[o]), uintBitsToFloat(apullv[o + 1u]), uintBitsToFloat(apullv[o + 2u]));
    aUv = vec2(uintBitsToFloat(apullv[o + 3u]), uintBitsToFloat(apullv[o + 4u]));
    aNormal = vec3(uintBitsToFloat(apullv[o + 7u]), uintBitsToFloat(apullv[o + 8u]), uintBitsToFloat(apullv[o + 9u]));
    aBone = CaneBytes(apullv[o + 10u]);
    aWeight = unpackUnorm4x8(apullv[o + 11u]);
    uint c = caneVertexId * 2u;
    aRgbaA = vec4(CaneBytes(apullc[c]));
    aRgbaB = vec4(CaneBytes(apullc[c + 1u]));
}
#define CANE_VERTEX_ID caneVertexId
#else
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aUv;
layout (location = 2) in vec4 aRgbaA;   // unnormalized bytes
layout (location = 3) in vec4 aRgbaB;
layout (location = 4) in vec3 aNormal;
layout (location = 5) in uvec4 aBone;   // skinning: the group's bone indices
layout (location = 6) in uint aDraw;    // the draw index (instanced, from the command's baseInstance)
layout (location = 7) in vec4 aWeight;  // and their weights (raw / 255, P_G 726)
#define CANE_VERTEX_ID uint(gl_VertexID)
#endif

// Per-frame draw data (render.cpp RECGPU / DRAWGPU).
struct REC
{
    mat4 model;
    vec4 fog;       // CM_BuildFogRecip 0x14E270: near (cm+0x240), far/near, max fog (cm+0x248 x uFog), enabled
    uvec4 bone;     // skinned groups: the first bone matrix, their count (0: not skinned); z: the vertices' base
    vec4 spec0;     // P_G 638 view-angle alpha: g0, g1, g2 (x 1/127, 1/16129), kind (0 view direction, 1 from a point, 2 per vertex)
    vec4 spec1;     // its clamp lo, hi, on
    vec4 specPos;   // kind 1's point (the group's space)
    uvec4 warp;     // space warps (wr.h): the state vectors' first + 1 (0: none), vertex count, first matrix, count | flags
};
layout (std430, binding = 0) readonly buffer RECS { REC arec[]; };
// The frame's bone matrices (animation code, bone.cpp: each bone's record world x its binding, 0x1684D0); a skinned
// vertex (in the group's owner space) is the weighted sum of its bones' transforms (P_G 726).
layout (std430, binding = 2) readonly buffer BONES { mat4 abone[]; };
// The space warps (WR, cid 0x73): per warped vertex and matrix a state vector; per WR four position and four
// coordinate matrices (glm columns = the original's rows). The VU program adds sum(matrix x state) to the vertex.
layout (std430, binding = 3) readonly buffer WARPS { vec4 awarp[]; };
layout (std430, binding = 4) readonly buffer WARPMATS { mat4 amatWarp[]; };
layout (std430, binding = 1) readonly buffer DRAWS { vec4 adraw[]; };   // SAA du, dv (P_G 421 adds it to every ST), object alpha (qw8.x), record
#ifdef BINDLESS
// (perf-3, ARB_bindless_texture) each pass draw's texture + sampler handle (render.cpp: 0 = the bound tex)
layout (std430, binding = 5) readonly buffer TEXHANDLES { uvec2 atexh[]; };
flat out uvec2 vTexh;
#endif
#ifdef TEXARRAY
// (perf-5, no bindless: render.cpp g_fGlobArrays) each pass draw's layer + 1 in the bound array texture (0: the bound tex)
layout (std430, binding = 5) readonly buffer TEXLAYERS { uvec2 alayer[]; };
flat out uint vLayer;
#endif

uniform mat4 matWorldToClip;
uniform int  icolor;         // 0: colour A, 1: colour B, 2: rgbaConst, 3: rgbaConst x vertex alpha
uniform vec4 rgbaConst;      // GS units
uniform vec3 posCamera;
uniform vec3 dirCamera;     // the camera's view direction (VU const 6)

noperspective out vec4 vColor;   // (the GS interpolates RGBA and F affinely in screen space; only STQ is perspective)
out vec2 vUv;
noperspective out float vFog;

void main()
{
#ifdef CANE_PULL
    CanePull();
#endif
    vec4 d = adraw[aDraw];
#ifdef BINDLESS
    vTexh = atexh[aDraw];
#endif
#ifdef TEXARRAY
    vLayer = alayer[aDraw].x;
#endif
    // (the record's fields read where they are used, not the whole record per vertex: a skinned vertex never reads the
    // model matrix, and only the view-angle groups read the spec fields)
    uint irec = floatBitsToUint(d.w);   // (no const: GLSL ES wants constant initializers there)
    vec4 fogParams = arec[irec].fog;
    uvec4 recBone = arec[irec].bone;
    uvec4 recWarp = arec[irec].warp;
    vec4 posWorld;
    if (recBone.y != 0u)
    {
        vec4 p = vec4(aPos, 1.0);
        posWorld = aWeight.x * (abone[recBone.x + min(aBone.x, recBone.y - 1u)] * p)
                 + aWeight.y * (abone[recBone.x + min(aBone.y, recBone.y - 1u)] * p)
                 + aWeight.z * (abone[recBone.x + min(aBone.z, recBone.y - 1u)] * p)
                 + aWeight.w * (abone[recBone.x + min(aBone.w, recBone.y - 1u)] * p);
        posWorld.w = 1.0;
    }
    else
        posWorld = arec[irec].model * vec4(aPos, 1.0);
    vec2 duv = vec2(0.0);
    if (recWarp.x != 0u)
    {
        uint ivtx = CANE_VERTEX_ID - recBone.z;
        uint cmat = recWarp.w & 0xFFu;
        vec3 dpos = vec3(0.0);
        for (uint imat = 0u; imat < cmat; ++imat)
        {
            vec4 st = awarp[recWarp.x - 1u + imat * recWarp.y + ivtx];
            dpos += (amatWarp[recWarp.z + imat] * st).xyz;
            duv += (amatWarp[recWarp.z + 4u + imat] * st).xy;
        }
        if ((recWarp.w & 0x100u) != 0u) posWorld.xyz += dpos;
        if ((recWarp.w & 0x200u) == 0u) duv = vec2(0.0);
    }
    gl_Position = matWorldToClip * posWorld;
    vec4 c = icolor == 0 ? aRgbaA : (icolor == 1 ? aRgbaB : rgbaConst);
    float aA = aRgbaA.a;
    if ((icolor == 0 || icolor == 3) && arec[irec].spec1.z != 0.0)
    {
        // P_G 638: the direction to the camera in the group's space (1909's matrix; 0: -view direction, 1: from the
        // point, 2: from the vertex), . the S8 normal (n x 127, truncated as 0x16A000 packs it), a quadratic gain
        // clamped, colour A's alpha x gain rounded (FTOI0 of a x g + 0.5)
        mat4 model = arec[irec].model;
        vec4 spec0 = arec[irec].spec0, spec1 = arec[irec].spec1;
        mat3 mInv = inverse(mat3(model));
        vec3 v;
        if (spec0.w == 0.0) v = mInv * -dirCamera;
        else
        {
            vec3 cam = mInv * (posCamera - model[3].xyz);
            v = spec0.w == 1.0 ? cam - arec[irec].specPos.xyz : cam - aPos;
        }
        v = normalize(v);
        float x = dot(v, trunc(aNormal * 127.0));
        float g = clamp(spec0.x + spec0.y * x + spec0.z * x * x, spec1.x, spec1.y);
        aA = floor(aA * g + 0.5);
        if (icolor == 0) c.a = aA;
    }
    if (icolor == 3) c.a *= aA / 128.0;   // a constant colour faded by the vertex alpha
    c.a *= d.z;
    vColor = c;
    vUv = aUv + d.xy + duv;
    vFog = 0.0;
    if (fogParams.w != 0.0)
    {
        // The VU's fog: F = a + b / x with x = depth / near, fitted through (1, 4080) and (far / near,
        // (1 - max) * 4080); the GS mixes FOGCOL by 1 - F / 4080 (F clamped to 0..255 x 16).
        float x = max(gl_Position.w / fogParams.x, 1.0);
        float k = 1.0 - 1.0 / fogParams.y;
        vFog = k > 0.0 ? clamp(fogParams.z * (1.0 - 1.0 / x) / k, 0.0, 1.0) : 0.0;
    }
}
