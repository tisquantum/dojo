// A primitive's pixel, as the console's GPU colours it: the texel through the texture window, page and
// colour table, transparent when 0, modulated by the vertex colour unless raw.
#version 450

layout(push_constant) uniform Push {
    vec4 area;
    vec4 extra;
};

layout(set = 0, binding = 0) uniform usampler2D vram;

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vTexcoord;
layout(location = 2) flat in uvec4 vTexture; // page x | y << 16, table x | y << 16, depth | flags << 8, window

layout(location = 0) out vec4 outColor;

uint Fetch(ivec2 p)
{
    return texelFetch(vram, ivec2(p.x & 1023, p.y & 511), 0).r;
}

void main()
{
    uint flags = vTexture.z >> 8;
    int pass = int(extra.x);
    vec3 rgb = vColor.rgb;

    if ((flags & 1u) != 0u) {
        ivec2 t = ivec2(floor(vTexcoord));
        ivec2 windowMask = ivec2(vTexture.w & 0xFFu, (vTexture.w >> 8) & 0xFFu);
        ivec2 windowOffset = ivec2((vTexture.w >> 16) & 0xFFu, vTexture.w >> 24);
        ivec2 page = ivec2(vTexture.x & 0xFFFFu, vTexture.x >> 16);
        ivec2 table = ivec2(vTexture.y & 0xFFFFu, vTexture.y >> 16);
        uint depth = vTexture.z & 0xFFu;
        uint texel;

        t = ((t & ~windowMask) | (windowOffset & windowMask)) & 255;
        if (depth == 0u) {
            uint word = Fetch(page + ivec2(t.x >> 2, t.y));
            texel = Fetch(table + ivec2(int((word >> uint((t.x & 3) * 4)) & 15u), 0));
        } else if (depth == 1u) {
            uint word = Fetch(page + ivec2(t.x >> 1, t.y));
            texel = Fetch(table + ivec2(int((word >> uint((t.x & 1) * 8)) & 255u), 0));
        } else {
            texel = Fetch(page + t);
        }
        if (texel == 0u) {
            discard;
        }
        bool semiTexel = (texel & 0x8000u) != 0u;
        if (pass == 0 && (flags & 4u) != 0u && semiTexel) {
            discard;
        }
        if (pass == 1 && !semiTexel) {
            discard;
        }
        vec3 texelColor = vec3(texel & 31u, (texel >> 5) & 31u, (texel >> 10) & 31u) / 31.0;
        rgb = (flags & 2u) != 0u ? texelColor : min(texelColor * vColor.rgb * (255.0 / 128.0), vec3(1.0));
    }
    outColor = vec4(rgb, 1.0);
}
