// A rectangle in clip space, from the push constants, as a four-vertex strip.
#version 450

layout(push_constant) uniform Push {
    vec4 rect;  // clip-space corners: x0, y0, x1, y1
    vec4 extra;
};

layout(location = 0) out vec2 vCorner;

void main()
{
    vec2 corner = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
    gl_Position = vec4(mix(rect.xy, rect.zw, corner), 0.0, 1.0);
    vCorner = corner;
}
