#version 450
// a triangle over the lower-left half of the image (vktest.c), no vertex buffer
void main()
{
    const vec2 p[3] = vec2[3](vec2(-1.0, 1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
