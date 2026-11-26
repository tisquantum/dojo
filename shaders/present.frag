// The shown picture: a region of a source, drawn to the window in one of the display modes (vk_renderer.h):
//   0 bilinear: as drawn, filtered;
//   1 smooth: each window pixel the average of the source pixels it covers, which is supersampling when the
//     picture is drawn at a higher resolution than the window shows;
//   2 sharp pixels: a picture at the console's resolution (downsample.frag) scaled up with square pixels,
//     only the edges between them blended;
//   3 scanlines, 4 aperture grille, 5 shadow mask: a picture at the console's resolution as a CRT showed
//     it: each line a beam whose spot widens with its brightness, the gaps between lines dark, and the
//     tube's phosphor pattern over it, at the window's own pixels.
#version 450

layout(push_constant) uniform Push {
    vec4 rect;
    vec4 extra;  // the source region in normalized coordinates: u0, v0, u1, v1
    vec4 params; // the mode, the screen's curvature (0: flat), and the picture's size in window pixels
};

layout(set = 0, binding = 1) uniform sampler2D source;

layout(location = 0) in vec2 vCorner;
layout(location = 0) out vec4 outColor;

const float CRT_GAMMA = 2.4;     // the tube's
const float DISPLAY_GAMMA = 2.2; // the window's

vec3 Smooth(vec2 uv)
{
    vec2 size = vec2(textureSize(source, 0));
    vec2 footprint = (extra.zw - extra.xy) * size / params.zw; // source pixels across one window pixel
    if (footprint.x <= 1.0 && footprint.y <= 1.0) {
        return texture(source, uv).rgb;
    }
    ivec2 taps = ivec2(clamp(ceil(footprint), 1.0, 8.0));
    vec2 stride = footprint / vec2(taps) / size;
    vec2 first = uv - 0.5 * footprint / size + 0.5 * stride;
    vec3 sum = vec3(0.0);
    for (int y = 0; y < taps.y; y++) {
        for (int x = 0; x < taps.x; x++) {
            sum += pow(texture(source, first + stride * vec2(x, y)).rgb, vec3(DISPLAY_GAMMA));
        }
    }
    return pow(sum / float(taps.x * taps.y), vec3(1.0 / DISPLAY_GAMMA));
}

vec3 Sharp(vec2 uv)
{
    vec2 size = vec2(textureSize(source, 0));
    vec2 texels = (extra.zw - extra.xy) * size;
    vec2 prescale = max(floor(params.zw / texels), 1.0); // whole window pixels to a source pixel
    vec2 texel = uv * size;
    vec2 centre = fract(texel) - 0.5;
    vec2 range = 0.5 - 0.5 / prescale;
    vec2 f = (centre - clamp(centre, -range, range)) * prescale + 0.5;
    return texture(source, (floor(texel) + f) / size).rgb;
}

// The light along one of the tube's lines at source x (in pixels): the line's pixels, in linear light,
// blurred across as the beam's spot does. An interlaced picture's line is two of its rows.
vec3 Line(int line, int rowsPerLine, float x, ivec2 lo, ivec2 hi)
{
    float px = x - 0.5;
    float base = floor(px);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int i = -1; i <= 2; i++) {
        float tx = base + float(i);
        float d = px - tx;
        float w = exp2(-2.5 * d * d);
        ivec2 at = ivec2(clamp(int(tx), lo.x, hi.x), clamp(lo.y + line * rowsPerLine, lo.y, hi.y));
        vec3 c = pow(texelFetch(source, at, 0).rgb, vec3(CRT_GAMMA));
        if (rowsPerLine > 1) {
            c = 0.5 * (c + pow(texelFetch(source, ivec2(at.x, min(at.y + 1, hi.y)), 0).rgb, vec3(CRT_GAMMA)));
        }
        sum += w * c;
        total += w;
    }
    return sum / total;
}

vec3 Mask(int mode, vec2 pixel)
{
    const float dark = 0.55, light = 1.4;
    vec3 mask = vec3(dark);
    float phase;
    if (mode == 4) {
        // the aperture grille's vertical stripes
        phase = fract(floor(pixel.x) / 3.0);
    } else {
        // the shadow mask's staggered triads
        phase = fract((floor(pixel.x) + 3.0 * floor(pixel.y)) / 6.0);
    }
    if (phase < 0.333) {
        mask.r = light;
    } else if (phase < 0.666) {
        mask.g = light;
    } else {
        mask.b = light;
    }
    return mask;
}

vec3 Crt(vec2 corner, int mode)
{
    vec2 size = vec2(textureSize(source, 0));
    ivec2 lo = ivec2(extra.xy * size + 0.5);
    ivec2 hi = ivec2(extra.zw * size + 0.5) - 1;
    int rows = hi.y - lo.y + 1;
    int rowsPerLine = rows > 300 ? 2 : 1;
    int lines = rows / rowsPerLine;
    float x = float(lo.x) + corner.x * float(hi.x - lo.x + 1);
    float y = corner.y * float(lines);
    int line = int(floor(y));
    vec3 light = vec3(0.0);
    for (int i = -1; i <= 1; i++) {
        int l = clamp(line + i, 0, lines - 1);
        vec3 c = Line(l, rowsPerLine, x, lo, hi);
        float d = y - (float(line + i) + 0.5);
        // a bright line's spot is wider: it fills more of the gap
        float spot = mix(0.26, 0.42, sqrt(max(max(c.r, c.g), c.b)));
        light += c * exp(-0.5 * (d * d) / (spot * spot));
    }
    light *= mode == 3 ? 1.15 : 1.0;
    if (mode != 3) {
        light *= Mask(mode, gl_FragCoord.xy) * 1.2;
    }
    // a little of the picture's glow through the glass
    light += 0.06 * pow(texture(source, mix(extra.xy, extra.zw, corner)).rgb, vec3(CRT_GAMMA));
    return pow(clamp(light, 0.0, 1.0), vec3(1.0 / DISPLAY_GAMMA));
}

void main()
{
    int mode = int(params.x);
    vec2 corner = vCorner;
    float edge = 1.0;
    if (mode >= 3 && params.y > 0.0) {
        // the tube's curved glass, darker towards its rounded corners
        vec2 p = corner * 2.0 - 1.0;
        p *= vec2(1.0 + p.y * p.y * params.y / 32.0, 1.0 + p.x * p.x * params.y / 24.0);
        corner = p * 0.5 + 0.5;
        if (any(lessThan(corner, vec2(0.0))) || any(greaterThan(corner, vec2(1.0)))) {
            outColor = vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }
        vec2 fade = smoothstep(vec2(0.0), vec2(0.02, 0.025), corner) *
                    smoothstep(vec2(0.0), vec2(0.02, 0.025), 1.0 - corner);
        edge = fade.x * fade.y;
    }
    vec2 uv = mix(extra.xy, extra.zw, corner);
    vec3 c;
    if (mode == 1) {
        c = Smooth(uv);
    } else if (mode == 2) {
        c = Sharp(uv);
    } else if (mode >= 3) {
        c = Crt(corner, mode);
    } else {
        c = texture(source, uv).rgb;
    }
    outColor = vec4(c * edge, 1.0);
}
