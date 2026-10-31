#version 450

layout(push_constant) uniform PushConstants {
    vec2 viewport;
} pc;

layout(location = 0) in vec2 in_position;
layout(location = 1) in float in_depth;
layout(location = 2) in vec2 in_texcoord;
layout(location = 3) in vec4 in_color;
layout(location = 4) in float in_w;
layout(location = 5) in float in_q;

layout(location = 0) out vec3 out_texcoord;
layout(location = 1) out vec4 out_color;

void main() {
    const vec2 ndc = vec2(in_position.x / pc.viewport.x * 2.0 - 1.0,
                          in_position.y / pc.viewport.y * 2.0 - 1.0);
    gl_Position = vec4(ndc * in_w, in_depth / 65535.0 * in_w, in_w);
    out_texcoord = vec3(in_texcoord, in_q);
    out_color = in_color;
}
