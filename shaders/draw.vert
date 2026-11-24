// A primitive's vertex: video memory coordinates to the surface's clip space.
#version 450

layout(push_constant) uniform Push {
    vec4 area;  // the surface in video memory units: origin x, y, width, height
    vec4 extra; // x: the pass (0 opaque texels of a semi-transparent primitive, 1 its blended texels, 2 all)
};

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inTexcoord;
layout(location = 3) in uvec4 inTexture;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vTexcoord;
layout(location = 2) flat out uvec4 vTexture;

void main()
{
    // The console's GPU evaluates a pixel at its integer corner; half a pixel on puts that corner at the
    // centre the rasterizer samples, so coverage and texture coordinates fall where the console's do.
    vec2 position = (inPosition + 0.5 - area.xy) / area.zw;
    gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
    vColor = inColor;
    vTexcoord = inTexcoord;
    vTexture = inTexture;
}
