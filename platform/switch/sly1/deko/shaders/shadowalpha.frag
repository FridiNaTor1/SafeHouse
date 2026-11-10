#version 430 core
// Sly1-Release/shadow.cpp's kShadowAlphaFragmentSource, for the Switch's offline set (cmake/switch_shaders_sly1.py):
// the game compiles it at run time elsewhere. Keep the two in step. (The compiler assigns the sampler's binding: 0, the
// unit shadow.cpp binds the staging layer on.)
uniform sampler2DArray sourceLayer;
out vec4 FragColor;
void main()
{
    // texelFetch reads each staging texel unfiltered, so every layer texel is the bit-exact 8-bit alpha the RGBA8
    // layer used to hold.
    FragColor = vec4(texelFetch(sourceLayer, ivec3(gl_FragCoord.xy, 0), 0).a);
}
