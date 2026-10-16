#version 450

// Emits a single triangle that covers the entire viewport.
layout(location = 0) out vec2 outUV;

void main()
{
    vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    outUV = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
