#version 450

layout(push_constant) uniform PushConstants {
    vec2 viewport;
    int atest_func;
    float atest_ref;
    int fog_enable;
    vec3 fog_color;
    // Texture combine: low 3 bits func (0-4), bit 3 RGBA mode,
    // bit 4 color double; env holds TEXENVCOLOR as raw bits.
    int tex_func;
    uint tex_env;
} pc;

layout(binding = 0) uniform sampler2D texture_sampler;

layout(location = 0) in vec3 in_texcoord;
layout(location = 1) in vec4 in_color;
layout(location = 2) in float in_fog;

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
    // Mirror the software rasterizer: q == 0 means 1, otherwise divide by
    // q as-is (negative q flips s/t back into range; max(q, eps) garbage
    // turned every negative-q texel into an edge/wrap sample).
    float q = in_texcoord.z == 0.0 ? 1.0 : in_texcoord.z;
    vec4 texel = texture(texture_sampler, in_texcoord.xy / q);
    // Mirror the software combine in 255-space so both backends agree
    // to float epsilon (the parity gate allows 1 LSB).
    vec3 vc = floor(in_color.rgb * 255.0 + 0.5);
    vec3 tc = floor(texel.rgb * 255.0 + 0.5);
    float va = floor(in_color.a * 255.0 + 0.5);
    float ta = floor(texel.a * 255.0 + 0.5);
    vec3 env = vec3(float(pc.tex_env & 255u), float((pc.tex_env >> 8u) & 255u),
                    float((pc.tex_env >> 16u) & 255u));
    int func = pc.tex_func & 7;
    bool rgba = (pc.tex_func & 8) != 0;
    float dbl = ((pc.tex_func & 16) != 0) ? 2.0 : 1.0;
    vec3 rgb = vc;
    float alpha = va;
    if (func == 0) {
        rgb = (vc + 1.0) * tc * dbl / 256.0;
        if (rgba) alpha = (va + 1.0) * ta / 256.0;
    } else if (func == 1) {
        if (rgba) rgb = ((vc + 1.0) * (255.0 - ta) + (tc + 1.0) * ta) * dbl / 256.0;
        else rgb = tc * dbl;
    } else if (func == 2) {
        rgb = ((255.0 - tc) * vc + tc * env + 255.0) * dbl / 256.0;
        if (rgba) alpha = (va + 1.0) * ta / 256.0;
    } else if (func == 3) {
        rgb = tc * dbl;
        if (rgba) alpha = ta;
    } else {
        rgb = vc + tc * dbl;
        if (rgba) alpha = (va + 1.0) * ta / 256.0;
    }
    out_color = vec4(clamp(rgb, 0.0, 255.0) / 255.0, clamp(alpha, 0.0, 255.0) / 255.0);
    if (!alpha_pass(out_color.a)) discard;
    if (pc.fog_enable != 0) {
        float fog = clamp(in_fog, 0.0, 1.0);
        out_color.rgb = mix(pc.fog_color, out_color.rgb, fog);
    }
}
