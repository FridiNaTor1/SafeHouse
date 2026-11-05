#version 430 core

layout(location = 0) in vec4 vertexColor;

layout(location = 0) out vec4 FragColor;

void main()
{
    // Shadow layers are single-channel: glob.frag reads this red value back
    // as alpha through the array's swizzle.
    FragColor = vec4(vertexColor.a, 0.0, 0.0, vertexColor.a);
}
