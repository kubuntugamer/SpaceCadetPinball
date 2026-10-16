#version 450

// Separable gaussian blur used for the bloom ping-pong passes.

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D srcTex;

layout(push_constant) uniform Push {
    vec4 params; // xy = direction * texel size
} pc;

const float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

void main()
{
    vec2 dir = pc.params.xy;
    vec3 sum = texture(srcTex, inUV).rgb * weights[0];
    for (int i = 1; i < 5; i++)
    {
        vec2 off = dir * float(i);
        sum += texture(srcTex, inUV + off).rgb * weights[i];
        sum += texture(srcTex, inUV - off).rgb * weights[i];
    }
    outColor = vec4(sum, 1.0);
}
