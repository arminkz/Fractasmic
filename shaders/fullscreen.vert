#version 450

// One triangle that covers the whole viewport, generated from the vertex index
// so the pipeline needs no vertex buffer.
void main() {
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
