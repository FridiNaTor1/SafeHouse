#version 430 core
// A full-screen textured quad (Sly1-Release screen.vert's role), picture rows top first.
out vec2 vUv;
void main()
{
    const vec2 a[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(0, 1), vec2(1, 0), vec2(1, 1));
    vec2 p = a[gl_VertexID];
    vUv = vec2(p.x, 1.0 - p.y);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
