#version 430 core

layout(location = 0) in vec4 vertexColor;
#ifdef DEKO3D
in float vertexAlpha;
flat in float triangleAlpha;
uniform int constantAlpha;
#endif

layout(location = 0) out vec4 FragColor;

void main()
{
    // Shadow layers are single-channel: glob.frag reads this red value back
    // as alpha through the array's swizzle.
#ifdef DEKO3D
    // Flat alpha for sub-meshes whose vertices share one (render.cpp DrawDysh)
    float alpha = constantAlpha != 0 ? triangleAlpha : vertexAlpha;
    FragColor = vec4(alpha, 0.0, 0.0, alpha);
#else
    FragColor = vec4(vertexColor.a, 0.0, 0.0, vertexColor.a);
#endif
}
