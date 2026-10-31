#version 450

layout(binding = 0) uniform sampler2D source_texture;
layout(push_constant) uniform PushConstants {
    vec2 source_size;
    vec2 output_size;
} pc;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() {
    const vec2 texel = floor(in_uv * pc.output_size) / pc.output_size;
    out_color = texture(source_texture, texel);
}
