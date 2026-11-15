#version 450
layout(location = 0) noperspective out vec4 gsColor;
layout(location = 1) out vec2 gsUv;
layout(location = 2) noperspective out float gsFog;
void main() {
  vec2 pos[3] = vec2[3](vec2(-0.14, -0.17), vec2(0.14, -0.17), vec2(0, 0.17));
  float ws[3] = float[3](0.6, 1.0, 1.4);
  vec4 colors[3] = vec4[3](vec4(0.8, 0.1, 0.2, 1), vec4(0.2, 0.8, 0.1, 1),
                           vec4(0.1, 0.2, 0.8, 1));
  int v = gl_VertexIndex;
  int code = gl_InstanceIndex;
  int digit = (code / (v == 0 ? 1 : v == 1 ? 3 : 9)) % 3;
  float z = digit == 0 ? 0.2 : digit == 1 ? 1.0 : 1.4;
  vec2 center =
      vec2(-0.8 + 0.4 * float(code % 5), -0.83 + 0.33 * float(code / 5));
  gl_Position = vec4((pos[v] + center) * ws[v], z * ws[v], ws[v]);
  gsColor = colors[v];
  gsUv = vec2(float(v) * 0.03, 0);
  gsFog = float(v) * 0.02;
}
