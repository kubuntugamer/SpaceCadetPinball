#version 450

// Final composite: the sharp scene plus the additive bloom glow.
// Rim lighting makes the ball appear to have a reflective surface.

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D sceneTex;
layout(binding = 1) uniform sampler2D bloomTex;

// Ball rim lighting parameters (set from C++ side)
layout(push_constant) uniform Push {
    vec4 params; // x = bloom intensity, y = ball radius, z = ball center x, w = ball center y
} pc;

void main()
{
    vec3 scene = texture(sceneTex, inUV).rgb;
    vec3 bloom = texture(bloomTex, inUV).rgb;
    
    // Rim lighting: make the ball appear reflective
    float ballRadius = pc.params.y;
    vec2 ballCenter = vec2(pc.params.z, pc.params.w);
    vec2 fragCoord = inUV * vec2(600.0, 416.0); // screen dimensions
    float distToCenter = length(fragCoord - ballCenter);
    
    float rim = 0.0;
    if (distToCenter < ballRadius) {
        // Stronger rim at the edge, fall off toward center
        rim = (1.0 - distToCenter / ballRadius) * 0.5;
    }
    
    vec3 result = scene + bloom * pc.params.x;
    // Add rim only to the bright parts (the lights on the table)
    // Scale rim by scene luminance to keep it subtle on dark areas
    float lum = dot(scene, vec3(0.2126, 0.7152, 0.0722));
    rim *= smoothstep(0.3, 0.8, lum);
    
    outColor = vec4(result + rim * vec3(1.0, 0.95, 0.85), 1.0);
}
