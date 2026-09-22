#version 440

// A full-screen triangle from the vertex index alone: no vertex buffer.

layout(location = 0) out vec2 vNdc;

void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
    vNdc = p;
    gl_Position = vec4(p, 0.0, 1.0);
}
