// The shown picture brought down to the console's resolution: each pixel the average of the scale-by-scale
// block of the surface it covers (a box filter, in linear light), so a picture drawn at a high resolution
// keeps its smooth edges at the console's size. The CRT looks and the sharp pixels draw from this.
#version 450

layout(push_constant) uniform Push {
    vec4 rect;
    vec4 extra; // the region's first pixel in the source (x, y) and the source's scale (z)
};

layout(set = 0, binding = 1) uniform sampler2D source;

layout(location = 0) out vec4 outColor;

void main()
{
    int scale = int(extra.z);
    ivec2 base = ivec2(extra.xy) + ivec2(gl_FragCoord.xy) * scale;
    vec3 sum = vec3(0.0);
    for (int y = 0; y < scale; y++) {
        for (int x = 0; x < scale; x++) {
            sum += pow(texelFetch(source, base + ivec2(x, y), 0).rgb, vec3(2.2));
        }
    }
    outColor = vec4(pow(sum / float(scale * scale), vec3(1.0 / 2.2)), 1.0);
}
