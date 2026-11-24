// The shown picture: a region of a surface, filtered to the window.
#version 450

layout(push_constant) uniform Push {
    vec4 rect;
    vec4 extra; // the source region in normalized coordinates: u0, v0, u1, v1
};

layout(set = 0, binding = 1) uniform sampler2D source;

layout(location = 0) in vec2 vCorner;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(texture(source, mix(extra.xy, extra.zw, vCorner)).rgb, 1.0);
}
