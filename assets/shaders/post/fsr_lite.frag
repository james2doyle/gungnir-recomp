#version 450

layout(binding = 0) uniform sampler2D source_texture;
layout(push_constant) uniform PushConstants {
    vec2 source_size;
    vec2 output_size;
    float sharpness;
} pc;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() {
    const vec2 texel = 1.0 / pc.source_size;
    const vec3 center = texture(source_texture, in_uv).rgb;
    const vec3 average = (texture(source_texture, in_uv + vec2(texel.x, 0.0)).rgb +
                          texture(source_texture, in_uv - vec2(texel.x, 0.0)).rgb +
                          texture(source_texture, in_uv + vec2(0.0, texel.y)).rgb +
                          texture(source_texture, in_uv - vec2(0.0, texel.y)).rgb) * 0.25;
    out_color = vec4(clamp(center + pc.sharpness * (center - average), 0.0, 1.0), 1.0);
}
