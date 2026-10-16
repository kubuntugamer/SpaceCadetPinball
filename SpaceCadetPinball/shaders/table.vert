#version 450

// Push constants for drawing the software-rendered game vScreen as a textured quad.
layout(push_constant) uniform Push {
    vec4 dst; // x, y, width, height of the destination rectangle, in framebuffer pixels
    vec4 uv;  // u0, v0, u1, v1 texture coordinates
    vec4 fx;  // x = time in seconds, y = sparkle amount, z/w reserved
    vec2 fb;  // framebuffer width, height in pixels
} pc;

layout(location = 0) out vec2 outUV;

const vec2 corners[4] = vec2[4](
    vec2(0.0, 0.0),
    vec2(1.0, 0.0),
    vec2(0.0, 1.0),
    vec2(1.0, 1.0)
);

void main()
{
    vec2 c = corners[gl_VertexIndex];
    vec2 pixel = pc.dst.xy + c * pc.dst.zw;
    vec2 ndc = (pixel / pc.fb) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    outUV = mix(pc.uv.xy, pc.uv.zw, c);
}
