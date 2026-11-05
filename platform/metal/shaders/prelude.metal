#include <metal_stdlib>
using namespace metal;

#define UNIFORMS buffer(CANE_MTL_UNIFORM_BUFFER_SLOT)
#define SSBO(n) buffer(CANE_MTL_STORAGE_BUFFER_BASE + (n))

// Offscreen targets keep OpenGL's row order (row 0 at the bottom), so clip y
// is mirrored for them; the drawable is drawn in Metal's native orientation.
#define CANE_FINISH_POSITION(u, p) \
  float4((p).x, (p).y * (u).cane_clip.x, (p).z, (p).w)

struct RasterTarget {
  float4 position [[position]];
};

inline float3x3 CaneInverse3x3(float3x3 m) {
  float3 c0 = m[0];
  float3 c1 = m[1];
  float3 c2 = m[2];
  float3 r0 = cross(c1, c2);
  float3 r1 = cross(c2, c0);
  float3 r2 = cross(c0, c1);
  float det = dot(c0, r0);
  return transpose(float3x3(r0, r1, r2)) * (1.0 / det);
}
