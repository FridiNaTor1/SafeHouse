#version 430 core
// Sly 2 glob fragments: the GS's texture function (MODULATE, TCC 1) and alpha test, for one pass of a shader.
// The pass's ALPHA blend and depth test are GL state (render.cpp).

uniform sampler2D tex;
uniform int  fTextured;
uniform int  ate;            // TEST: alpha test enable
uniform int  atst;           //       0 never 1 always 2 less 3 lequal 4 equal 5 gequal 6 greater 7 notequal
uniform float aref;          //       reference (GS units)
uniform int  afailMode;      // 0: no test, 1: keep fragments that pass, 2: keep fragments that fail
uniform vec4 fogColor;       // 0..1

in vec4 vColor;
in vec2 vUv;
in float vFog;

out vec4 FragColor;

bool AlphaPasses(float a)
{
    switch (atst)
    {
    case 0: return false;
    case 1: return true;
    case 2: return a < aref;
    case 3: return a <= aref;
    case 4: return a == aref;
    case 5: return a >= aref;
    case 6: return a > aref;
    default: return a != aref;
    }
}

void main()
{
    vec4 c = vColor;
    if (fTextured != 0)
    {
        vec4 t = texture(tex, vUv) * 255.0;
        c = floor(t * vColor / 128.0);
    }
    c = clamp(c, 0.0, 255.0);
    if (afailMode != 0 && ate != 0)
    {
        bool fPass = AlphaPasses(c.a);
        if ((afailMode == 1) != fPass) discard;
    }
    vec3 rgb = c.rgb / 255.0;
    rgb = mix(rgb, fogColor.rgb, vFog);
    // The GS blends with As / 128 (0x80 = 1.0).
    FragColor = vec4(rgb, min(c.a / 128.0, 1.0));
}
