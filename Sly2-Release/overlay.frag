#version 430 core
// orig0x1477D8: divide interpolated ST by Q, then map the nominal1024x1024
// scratch UVs to the native two-frame atlas, stored bottom row first by GL.
noperspective in vec3 vStq;
uniform sampler2D tex;
out vec4 FragColor;
void main()
{
    vec2 uv = vStq.xy / vStq.z;
    vec2 atlas = vec2(uv.x * 2.0, 1.0 - uv.y * (1024.0 / 224.0));
    FragColor = vec4(texture(tex, atlas).rgb, 1.0);
}
