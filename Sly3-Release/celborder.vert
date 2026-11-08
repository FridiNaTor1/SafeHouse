#version 430 core
// Cel borders (mesh type 1), adapted from Sly1-Release celborder.vert: per edge {E0, E1, OA, OB} the silhouette
// test of the VU1 program P_A (the two faces on either side of the edge face opposite ways on screen), drawn
// as a screen-space quad of the edge (P_A offsets along the edge's normal) instead of a GL line.

layout(std430, binding = 8) readonly buffer EDGEBUFFER { vec4 edges[]; };


uniform mat4 matWorldToClip;
uniform mat4 matModel;
uniform vec2 halfWidthNdc;   // half the border width in NDC units (x, y)
uniform int edgeBase;

// Skinned borders (animation code): per edge vertex its weights and the group's bone indices, the frame's bone
// matrices (P_A 60: the positions skinned with up to 4 bones).
struct EDGESKIN { vec4 w; uvec4 b; };
layout(std430, binding = 9) readonly buffer EDGESKINBUFFER { EDGESKIN askin[]; };
layout(std430, binding = 2) readonly buffer BONES { mat4 abone[]; };
uniform int cbone;
uniform int iboneFirst;

vec4 PosWorld(int i)
{
    vec4 p = vec4(edges[i].xyz, 1.0);
    if (cbone <= 0) return matModel * p;
    EDGESKIN s = askin[i];
    uint c = uint(cbone) - 1u;
    uint f = uint(iboneFirst);
    vec4 q = s.w.x * (abone[f + min(s.b.x, c)] * p) + s.w.y * (abone[f + min(s.b.y, c)] * p)
           + s.w.z * (abone[f + min(s.b.z, c)] * p) + s.w.w * (abone[f + min(s.b.w, c)] * p);
    return vec4(q.xyz, 1.0);
}

void Cull() { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); }
float Cross2D(vec2 a, vec2 b) { return a.x * b.y - a.y * b.x; }

void main()
{
    int iedge = edgeBase + gl_VertexID / 6;
    int icorner = gl_VertexID % 6;
    vec4 A = matWorldToClip * PosWorld(iedge * 4 + 0);
    vec4 B = matWorldToClip * PosWorld(iedge * 4 + 1);
    vec4 C = matWorldToClip * PosWorld(iedge * 4 + 2);
    vec4 D = matWorldToClip * PosWorld(iedge * 4 + 3);
    if (A.w <= 0.0 || B.w <= 0.0 || C.w <= 0.0 || D.w <= 0.0) { Cull(); return; }
    vec2 a = A.xy / A.w, b = B.xy / B.w, c = C.xy / C.w, d = D.xy / D.w;
    // P_A 160..: facing of the two faces beside the edge in projected space; a silhouette when they differ.
    vec2 d2 = b - a;
    float facingA = Cross2D(a - c, d2);
    float facingB = Cross2D(d - b, d2);
    if (!(facingA * facingB < 0.0)) { Cull(); return; }
    // The line is pushed sideways, away from face A (side = sign of facing A), and extended at E1 along the
    // edge (P_A: +-0.00024 projected units); natively it is a quad one line wide on that side, in pixels.
    vec2 dirPx = d2 / halfWidthNdc;
    float len = length(dirPx);
    if (len < 1e-6) { Cull(); return; }
    vec2 unit = dirPx / len;
    float side = facingA < 0.0 ? -1.0 : 1.0;
    vec2 perp = vec2(unit.y, -unit.x) * side;
    // corners: 0 a, 1 a+, 2 b+, 3 a, 4 b+, 5 b   (+ = one line width outward)
    bool fB = icorner == 2 || icorner == 4 || icorner == 5;
    bool fOut = icorner == 1 || icorner == 2 || icorner == 4;
    vec4 P = fB ? B : A;
    vec2 off = (fOut ? perp * 2.0 : vec2(0.0)) + (fB ? unit : -unit) * 0.5;
    P.xy += off * halfWidthNdc * P.w;
    // At the edge's own depth, as the original (TEST 0x73001: ZTST GREATER, no offset): the quad lies outside the
    // silhouette, and any bias toward the camera lets edges hidden behind thin parts (fingers, lips) show through.
    gl_Position = P;
}
