// Copies video memory, as uploaded, into a surface at its scale: what the game loads straight into a frame
// buffer (backgrounds, fills) shows there.
#version 450

layout(push_constant) uniform Push {
    vec4 rect;
    vec4 extra; // the surface's origin in video memory (x, y) and its scale (z)
};

layout(set = 0, binding = 0) uniform usampler2D vram;

layout(location = 0) out vec4 outColor;

void main()
{
    ivec2 p = ivec2(floor(gl_FragCoord.xy / extra.z + extra.xy));
    uint texel = texelFetch(vram, ivec2(p.x & 1023, p.y & 511), 0).r;
    outColor = vec4(vec3(texel & 31u, (texel >> 5) & 31u, (texel >> 10) & 31u) / 31.0, 1.0);
}
