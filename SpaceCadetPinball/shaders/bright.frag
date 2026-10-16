#version 450

// Bright-pass: keeps only the energy above a luminance threshold so that the
// blur/composite stages produce a glow around the bright parts (the lights).

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D srcTex;

layout(push_constant) uniform Push {
    vec4 params; // x = threshold, y = soft knee
} pc;

void main()
{
    vec3 c = texture(srcTex, inUV).rgb;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float contrib = smoothstep(pc.params.x, pc.params.x + pc.params.y, l);
    outColor = vec4(c * contrib, 1.0);
}
