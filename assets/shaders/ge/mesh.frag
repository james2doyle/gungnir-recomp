#version 450

layout(binding = 0) uniform sampler2D texture_sampler;

layout(location = 0) in vec3 in_texcoord;
layout(location = 1) in vec4 in_color;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = texture(texture_sampler, in_texcoord.xy / max(in_texcoord.z, 1e-6)) * in_color;
}
