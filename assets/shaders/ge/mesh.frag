#version 450

layout(push_constant) uniform PushConstants {
    vec2 viewport;
    int atest_func;
    float atest_ref;
} pc;

layout(binding = 0) uniform sampler2D texture_sampler;

layout(location = 0) in vec3 in_texcoord;
layout(location = 1) in vec4 in_color;

layout(location = 0) out vec4 out_color;

bool alpha_pass(float a) {
    if (pc.atest_func < 0) return true;
    if (pc.atest_func == 0) return false;
    if (pc.atest_func == 1) return true;
    if (pc.atest_func == 2) return a == pc.atest_ref;
    if (pc.atest_func == 3) return a != pc.atest_ref;
    if (pc.atest_func == 4) return a < pc.atest_ref;
    if (pc.atest_func == 5) return a <= pc.atest_ref;
    if (pc.atest_func == 6) return a > pc.atest_ref;
    return a >= pc.atest_ref;
}

void main() {
    vec4 texel = texture(texture_sampler, in_texcoord.xy / max(in_texcoord.z, 1e-6));
    out_color = texel * in_color;
    if (!alpha_pass(out_color.a)) discard;
}
