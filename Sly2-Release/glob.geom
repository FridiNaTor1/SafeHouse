#version 430 core
// Sly 2 glob triangles clipped against the near plane the way the original's VU1 program does it, before the GS draws
// them (R51). The GS interpolates RGBA and F affinely in screen space (glob.vert: noperspective), but only between the
// vertices it is sent: P_G clips a triangle that crosses the near plane in 3D, so a new vertex on the plane carries the
// colour linear in the clip-space segment (the reference's GS log at Trojan Tank: alphas 104..117 on the clip vertices
// of the ramp under the camera, between its vertices' 70..127). GL's own clipper gives a noperspective output at a
// clip vertex the value that varies linearly in screen space over the unclipped triangle instead (GL 4.6 13.7), which
// for a vertex far behind the camera extrapolates: the floor under a low camera went dark and its frame alpha (the
// shadow combine's darkness) to 0. Here the near plane is w - z >= 0 (render.cpp: reversed-Z, z = near); a triangle
// wholly in front is passed through unchanged.

#if defined(CANE_BLEND1) && defined(BINDLESS)
#define CANE_DRAWSTATE 1   // (glob.vert)
#endif

layout (triangles) in;
layout (triangle_strip, max_vertices = 4) out;

noperspective in vec4 gsColor[];
in vec2 gsUv[];
noperspective in float gsFog[];
#ifdef BINDLESS
flat in uvec2 gsTexh[];
flat out uvec2 vTexh;
#endif
#ifdef CANE_DRAWSTATE
flat in uint gsDrawState[];
flat out uint vDrawState;
#endif
#ifdef TEXARRAY
flat in uint gsLayer[];
flat out uint vLayer;
#endif

noperspective out vec4 vColor;
out vec2 vUv;
noperspective out float vFog;

// (no dynamically indexed arrays: the inputs are read with constant indices and the clipped polygon is kept in named
// variables picked by selects. The polygon used to be a local array written at a dynamic index in the clipping branch:
// llvmpipe's OpenGL compiles that wrongly (the triangles wholly in front, the usual case, came out missing wherever a
// triangle crossing the plane shared their SIMD group), which matches the reports from AMD's Windows driver (Vulkan and
// OpenGL) and Adreno 750: the binocucom's speaker windows, whose close cameras cut through the characters, came out
// empty. On llvmpipe SLY2N_NEARCLIP=0, without this stage, drew them.)
vec4 P0, P1, P2;
vec4 C0, C1, C2;
vec2 U0, U1, U2;
float F0, F1, F2;
ivec2 aij0, aij1, aij2, aij3;   // the polygon's vertices: (i, j, t) = mix(vertex i, vertex j, t)
float at0, at1, at2, at3;

void Put(inout int c, int i, int j, float t)
{
    if (c == 0) { aij0 = ivec2(i, j); at0 = t; }
    else if (c == 1) { aij1 = ivec2(i, j); at1 = t; }
    else if (c == 2) { aij2 = ivec2(i, j); at2 = t; }
    else { aij3 = ivec2(i, j); at3 = t; }
    ++c;
}

vec4 PosOf(int i) { return i == 0 ? P0 : (i == 1 ? P1 : P2); }
vec4 ColorOf(int i) { return i == 0 ? C0 : (i == 1 ? C1 : C2); }
vec2 UvOf(int i) { return i == 0 ? U0 : (i == 1 ? U1 : U2); }
float FogOf(int i) { return i == 0 ? F0 : (i == 1 ? F1 : F2); }

void EmitAt(int i, int j, float t)
{
    gl_Position = mix(PosOf(i), PosOf(j), t);
    vColor = mix(ColorOf(i), ColorOf(j), t);
    vUv = mix(UvOf(i), UvOf(j), t);
    vFog = mix(FogOf(i), FogOf(j), t);
#ifdef BINDLESS
    vTexh = gsTexh[0];
#endif
#ifdef CANE_DRAWSTATE
    vDrawState = gsDrawState[0];
#endif
#ifdef TEXARRAY
    vLayer = gsLayer[0];
#endif
    EmitVertex();
}

void main()
{
    P0 = gl_in[0].gl_Position; P1 = gl_in[1].gl_Position; P2 = gl_in[2].gl_Position;
    C0 = gsColor[0]; C1 = gsColor[1]; C2 = gsColor[2];
    U0 = gsUv[0]; U1 = gsUv[1]; U2 = gsUv[2];
    F0 = gsFog[0]; F1 = gsFog[1]; F2 = gsFog[2];
    float d0 = P0.w - P0.z, d1 = P1.w - P1.z, d2 = P2.w - P2.z;
    bool f0 = d0 >= 0.0, f1 = d1 >= 0.0, f2 = d2 >= 0.0;
    // Sutherland-Hodgman against the one plane: up to four vertices in the triangle's order
    int c = 0;
    if (f0) Put(c, 0, 0, 0.0);
    if (f0 != f1) Put(c, 0, 1, d0 / (d0 - d1));
    if (f1) Put(c, 1, 1, 0.0);
    if (f1 != f2) Put(c, 1, 2, d1 / (d1 - d2));
    if (f2) Put(c, 2, 2, 0.0);
    if (f2 != f0) Put(c, 2, 0, d2 / (d2 - d0));
    if (c < 3) return;
    // One emission call site for both paths: Mali r44p1's Vulkan compiler crashes when
    // noperspective outputs are emitted from separate unclipped/clipped paths.
    // A polygon a b c [d] becomes the triangle strip a b [d] c.
    for (int k = 0; k < c; ++k)
    {
        int n = c == 4 && k >= 2 ? 5 - k : k;
        ivec2 ij = n == 0 ? aij0 : (n == 1 ? aij1 : (n == 2 ? aij2 : aij3));
        float t = n == 0 ? at0 : (n == 1 ? at1 : (n == 2 ? at2 : at3));
        EmitAt(ij.x, ij.y, t);
    }
    EndPrimitive();
}
