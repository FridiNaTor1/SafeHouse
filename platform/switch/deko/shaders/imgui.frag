#version 430 core
uniform sampler2D Texture;
in vec2 vUv;
in vec4 vColor;
out vec4 FragColor;
void main()
{
    FragColor = vColor * texture(Texture, vUv);
}
