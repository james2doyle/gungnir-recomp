#version 450

layout(binding = 0) uniform sampler2D source_texture;
layout(push_constant) uniform PushConstants {
    vec2 source_size;
    vec2 output_size;
    float strength;
    float period;
} pc;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() {
    vec4 color = texture(source_texture, in_uv);
    const vec2 pixel = floor(in_uv * pc.output_size);
    if (mod(pixel.x, pc.period) < 0.5 || mod(pixel.y, pc.period) < 0.5) color.rgb *= 1.0 - pc.strength;
    out_color = color;
}
