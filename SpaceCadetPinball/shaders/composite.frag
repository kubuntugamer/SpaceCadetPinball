#version 450

// Final composite: the sharp scene plus the additive bloom glow.

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D sceneTex;
layout(binding = 1) uniform sampler2D bloomTex;

layout(push_constant) uniform Push {
    vec4 params; // x = bloom intensity
} pc;

void main()
{
    vec3 scene = texture(sceneTex, inUV).rgb;
    vec3 bloom = texture(bloomTex, inUV).rgb;
    outColor = vec4(scene + bloom * pc.params.x, 1.0);
}
