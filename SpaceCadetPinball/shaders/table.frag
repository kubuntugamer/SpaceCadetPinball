#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D tableTex;

layout(push_constant) uniform Push {
    vec4 dst;
    vec4 uv;
    vec4 fx;  // x = time in seconds, y = sparkle amount, z/w reserved
    vec2 fb;
} pc;

float hash21(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

// Twinkling glint: one pseudo-random point per cell, each flaring briefly on its
// own schedule. Kept as a small round dot (no long streaks) so it reads as a
// sparkle on the lights rather than confetti.
float sparkle_layer(vec2 uv, float t, float density, float seed)
{
    vec2 g = uv * density;
    vec2 cell = floor(g);
    vec2 f = fract(g);

    float r = hash21(cell + seed);
    float s = sin(t * (1.5 + r * 5.0) + r * 6.2831853);
    s = smoothstep(0.58, 1.0, s); // lit for part of each cycle

    vec2 pt = vec2(hash21(cell + seed + 1.7), hash21(cell + seed + 9.3));
    float d = length(f - pt);
    return smoothstep(0.20, 0.0, d) * s;
}

void main()
{
    // The game's vScreen is a fully composited, opaque image. Force alpha to 1
    // so the ImGui/GUI layer drawn on top composites correctly.
    vec3 col = texture(tableTex, inUV).rgb;

    float amount = pc.fx.y;
    if (amount > 0.0)
    {
        // Only glint on the bright parts of the table (the lights), and fade the
        // effect in as those pixels get brighter.
        float lum = dot(col, vec3(0.2126, 0.7152, 0.0722));
        float mask = smoothstep(0.40, 0.80, lum);
        if (mask > 0.0)
        {
            float t = pc.fx.x;
            float s = sparkle_layer(inUV, t, 70.0, 0.0)
                    + sparkle_layer(inUV, t * 1.7 + 3.1, 120.0, 5.0) * 0.7
                    + sparkle_layer(inUV, t * 2.3 + 7.7, 190.0, 11.0) * 0.4;
            // Bounce the glint so the bright cores really pop, then clamp.
            float glint = min(s * 1.35, 1.0) * amount * mask;
            col += vec3(1.0, 0.97, 0.88) * glint;
        }
    }

    outColor = vec4(col, 1.0);
}
