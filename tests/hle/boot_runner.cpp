// Bounded Linux boot of the recompiled main module (plan T6c, TC-6.2).
//
// The boot flow lives in runtime/session/game_session.cpp and is shared with
// the Android runtime (T13a). This harness owns only CLI parsing, a counting
// audio sink and the JSON report.
//
// Exit: 0 reached the first frame buffer, 1 a missing/failed HLE import or
// guest fault stopped the run, 2 usage/input error, 3 bounded out before the
// first flip.

#include "audio/audio_sink.hpp"
#include "ge/software.hpp"
#include "hle/hle_media.hpp"
#include "input/input_state.hpp"
#include "session/game_session.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

class CountingSink final : public gungnir::audio::AudioSink {
public:
    bool submit(const gungnir::audio::AudioBuffer &buffer) override {
        ++buffers;
        frames += buffer.frames;
        for (std::size_t i = 0u; i < static_cast<std::size_t>(buffer.frames) * buffer.channels; ++i) {
            const int value = std::abs(static_cast<int>(buffer.samples[i]));
            if (value > peak) peak = value;
        }
        return true;
    }

    void reset() override {
        buffers = 0u;
        frames = 0u;
        peak = 0;
    }

    std::uint64_t buffers{};
    std::uint64_t frames{};
    int peak{};
};

struct PadPress {
    std::uint64_t start{};
    std::uint64_t hold{};
    gungnir::input::ButtonMask mask{};
};

bool button_from_name(const std::string &name, gungnir::input::ButtonMask &mask) {
    using gungnir::input::Button;
    static const std::pair<const char *, Button> names[] = {
        {"cross", Button::Cross}, {"circle", Button::Circle}, {"square", Button::Square},
        {"triangle", Button::Triangle}, {"start", Button::Start}, {"select", Button::Select},
        {"up", Button::Up}, {"down", Button::Down}, {"left", Button::Left}, {"right", Button::Right},
        {"l", Button::L}, {"r", Button::R},
    };
    for (const auto &[text, button] : names) {
        if (name == text) {
            mask |= gungnir::input::button_mask(button);
            return true;
        }
    }
    return false;
}

bool parse_pad_press(const std::string &text, PadPress &press) {
    const std::size_t first = text.find(':');
    const std::size_t second = first == std::string::npos ? std::string::npos : text.find(':', first + 1);
    try {
        press.start = std::stoull(text.substr(0, first));
        press.hold = second == std::string::npos ? 8u : std::stoull(text.substr(second + 1));
    } catch (const std::exception &) {
        return false;
    }
    const std::string button = first == std::string::npos
        ? std::string("cross")
        : text.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
    press.mask = 0u;
    return button_from_name(button, press.mask) && press.hold != 0u;
}

[[noreturn]] void usage(const char *message) {
    std::cerr << "boot_runner: " << message << "\n"
              << "usage: boot_runner --elf FILE --nids CSV --disc FILE --ms DIR\n"
                 "                   [--load-base HEX] [--max-dispatches N]\n"
                 "                   [--wall-seconds N] [--stall-seconds N] [--boot-path TEXT]\n"
                 "                   [--trace-out FILE] [--frame-json FILE] [--frame-at N] [--frame-ppm FILE]\n"
                 "                   [--texture-ppm FILE] [--draw-only N]\n"
                 "                   [--snapshot-out FILE] [--snapshot-in FILE]\n"
                 "                   [--no-stop-at-flip]\n";
    std::exit(2);
}

std::uint32_t parse_hex(const std::string &text) {
    return static_cast<std::uint32_t>(std::stoul(text, nullptr, 0));
}

} // namespace

int main(int argc, char **argv) {
    gungnir::session::Config config;
    std::string trace_path;
    std::string frame_path;
    std::string ppm_path;
    std::string texture_path;
    std::string snapshot_out;
    std::string snapshot_in;
    std::size_t draw_only = std::numeric_limits<std::size_t>::max();
    std::uint64_t frame_at = 60u;
    std::vector<PadPress> pad_presses;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char *name) -> std::string {
            if (i + 1 >= argc) usage((std::string(name) + " needs a value").c_str());
            return argv[++i];
        };
        if (arg == "--elf") config.elf_path = next("--elf");
        else if (arg == "--nids") config.nids_path = next("--nids");
        else if (arg == "--disc") config.disc_path = next("--disc");
        else if (arg == "--ms") config.memory_stick = next("--ms");
        else if (arg == "--load-base") config.load_base = parse_hex(next("--load-base"));
        else if (arg == "--max-dispatches") config.max_dispatches = std::stoull(next("--max-dispatches"));
        else if (arg == "--wall-seconds") config.wall_seconds = std::stod(next("--wall-seconds"));
        else if (arg == "--stall-seconds") config.stall_seconds = std::stod(next("--stall-seconds"));
        else if (arg == "--boot-path") config.boot_path = next("--boot-path");
        else if (arg == "--trace-out") trace_path = next("--trace-out");
        else if (arg == "--frame-json") frame_path = next("--frame-json");
        else if (arg == "--frame-at") frame_at = std::stoull(next("--frame-at"));
        else if (arg == "--frame-ppm") ppm_path = next("--frame-ppm");
        else if (arg == "--texture-ppm") texture_path = next("--texture-ppm");
        else if (arg == "--draw-only") draw_only = std::stoull(next("--draw-only"));
        else if (arg == "--snapshot-out") snapshot_out = next("--snapshot-out");
        else if (arg == "--snapshot-in") snapshot_in = next("--snapshot-in");
        else if (arg == "--pad-press") {
            PadPress press;
            if (!parse_pad_press(next("--pad-press"), press)) usage("bad --pad-press");
            pad_presses.push_back(press);
        }
        else if (arg == "--no-stop-at-flip") config.stop_at_flip = false;
        else usage(("unknown argument: " + arg).c_str());
    }
    if (config.elf_path.empty() || config.nids_path.empty())
        usage("--elf and --nids are required");
    if (config.max_dispatches == 0u) usage("--max-dispatches must be non-zero");

    try {
        CountingSink audio_sink;
        gungnir::session::GameSession session;
        session.set_diagnostic_sink([](std::string_view line) {
            std::cerr << "[session] " << line << "\n";
        });
        std::string error;
        if (!session.start(config, &audio_sink, error)) {
            std::cerr << "boot_runner: " << error << "\n";
            return 1;
        }
        if (!pad_presses.empty()) {
            gungnir::hle::set_display_flip_observer([&pad_presses] {
                const std::uint64_t flip = gungnir::hle::display_state().flips;
                gungnir::input::ButtonMask mask = 0u;
                for (const PadPress &press : pad_presses)
                    if (flip >= press.start && flip < press.start + press.hold) mask |= press.mask;
                gungnir::input::state().set_buttons(gungnir::input::Source::Keyboard, 0, 0, mask);
            });
        }
        std::vector<gungnir::ge::Draw> first_frame;
        if (!frame_path.empty()) {
            gungnir::hle::set_ge_frame_sink([&first_frame, frame_at](const std::vector<gungnir::ge::Draw> &draws) {
                if (first_frame.empty() && gungnir::hle::ge_frame_count() >= frame_at && !draws.empty())
                    first_frame = draws;
            });
        }
        if (!snapshot_in.empty() && !session.load_snapshot(snapshot_in, error)) {
            std::cerr << "boot_runner: " << error << "\n";
            return 1;
        }
        session.run();
        session.runtime().report_hle_histogram(25u);
        if (!snapshot_out.empty() && !session.save_snapshot(snapshot_out, error)) {
            std::cerr << "boot_runner: " << error << "\n";
            return 1;
        }

        const gungnir::session::Result &result = session.result();
        if (!result.error.empty()) {
            std::cerr << "boot_runner: " << result.error << "\n";
            return 1;
        }

        std::ofstream out;
        if (!trace_path.empty()) {
            out.open(trace_path);
            if (!out) usage("cannot open trace output");
        }
        std::ostream &report = trace_path.empty() ? std::cout : out;
        const gungnir::session::AudioStats audio{audio_sink.buffers, audio_sink.frames, audio_sink.peak};
        report << gungnir::session::result_json(result, &audio);
        if (out) out.close();

        if (!frame_path.empty()) {
            std::ofstream frame_out(frame_path);
            if (!frame_out) usage("cannot open frame output");
            std::size_t vertices = 0u;
            std::size_t indices = 0u;
            std::size_t textured = 0u;
            for (const gungnir::ge::Draw &draw : first_frame) {
                vertices += draw.vertices.size();
                indices += draw.indices.size();
                if (draw.state.texture != nullptr) ++textured;
            }
            frame_out << "{\n"
                      << "  \"draws\": " << first_frame.size() << ",\n"
                      << "  \"vertices\": " << vertices << ",\n"
                      << "  \"indices\": " << indices << ",\n"
                      << "  \"textured_draws\": " << textured << ",\n"
                      << "  \"ge_frames\": " << gungnir::hle::ge_frame_count() << ",\n"
                      << "  \"ge_gaps\": " << gungnir::hle::ge_gap_count();
            if (!first_frame.empty()) {
                const gungnir::ge::Draw &draw = first_frame.front();
                frame_out << ",\n  \"first_draw\": {";
                frame_out << "\"vertices\": " << draw.vertices.size() << ", "
                          << "\"indices\": " << draw.indices.size() << ", "
                          << "\"scissor\": [" << draw.state.scissor_x0 << ", " << draw.state.scissor_y0
                          << ", " << draw.state.scissor_x1 << ", " << draw.state.scissor_y1 << "], "
                          << "\"textured\": " << (draw.state.texture != nullptr ? "true" : "false");
                if (!draw.vertices.empty()) {
                    const gungnir::ge::ScreenVertex &v = draw.vertices.front();
                    frame_out << ", \"v0\": [" << v.x << ", " << v.y << ", " << v.z << "], "
                              << "\"uv0\": [" << v.texcoord[0] << ", " << v.texcoord[1] << "]"
                              << ", \"color0\": [" << v.color[0] << ", " << v.color[1] << ", "
                              << v.color[2] << ", " << v.color[3] << "]";
                }
                if (draw.state.texture != nullptr) {
                    std::size_t nonzero = 0u;
                    for (std::uint32_t pixel : draw.state.texture->pixels)
                        if ((pixel & 0xFFFFFFu) != 0u) ++nonzero;
                    frame_out << ", \"texture\": [" << draw.state.texture->width << ", "
                              << draw.state.texture->height << "]"
                              << ", \"tex_nonzero\": " << nonzero
                              << ", \"tex0\": " << (draw.state.texture->pixels.empty() ? 0u
                                                                                       : draw.state.texture->pixels.front());
                }
                frame_out << "}";
            }
            frame_out << ",\n  \"draw_list\": [";
            for (std::size_t d = 0; d < first_frame.size(); ++d) {
                const gungnir::ge::Draw &draw = first_frame[d];
                if (d != 0u) frame_out << ", ";
                frame_out << "{\"vertices\": " << draw.vertices.size()
                          << ", \"indices\": " << draw.indices.size()
                          << ", \"primitive\": \"" << gungnir::ge::primitive_name(draw.primitive) << "\""
                          << ", \"target\": \"0x" << std::hex << draw.state.draw_buffer << std::dec << "\""
                          << ", \"clear\": " << (draw.state.clear ? "true" : "false")
                          << ", \"mask\": \"0x" << std::hex << draw.state.write_mask << std::dec << "\""
                          << ", \"scissor\": [" << draw.state.scissor_x0 << ", " << draw.state.scissor_y0
                          << ", " << draw.state.scissor_x1 << ", " << draw.state.scissor_y1 << "]"
                          << ", \"textured\": " << (draw.state.texture != nullptr ? "true" : "false");
                if (draw.state.texture != nullptr) {
                    std::size_t nonzero = 0u;
                    for (std::uint32_t pixel : draw.state.texture->pixels)
                        if ((pixel & 0xFFFFFFu) != 0u) ++nonzero;
                    frame_out << ", \"texture\": [" << draw.state.texture->width << ", "
                              << draw.state.texture->height << "]"
                              << ", \"tex_nonzero\": " << nonzero;
                }
                frame_out << ", \"blend\": " << (draw.state.blend.enabled ? "true" : "false")
                          << ", \"blend_factors\": [" << static_cast<int>(draw.state.blend.source) << ", "
                          << static_cast<int>(draw.state.blend.destination) << "]"
                          << ", \"blend_eq\": " << static_cast<int>(draw.state.blend.equation)
                          << ", \"fix\": [\"0x" << std::hex << draw.state.blend.fixed_source
                          << "\", \"0x" << draw.state.blend.fixed_destination << std::dec << "\"]"
                          << ", \"depth\": [" << (draw.state.depth_test ? "true" : "false") << ", "
                          << (draw.state.depth_write ? "true" : "false") << "]"
                          << ", \"cull\": " << static_cast<int>(draw.state.cull);
                if (!draw.vertices.empty()) {
                    frame_out << ", \"verts\": [";
                    for (std::size_t vi = 0; vi < draw.vertices.size(); ++vi) {
                        const gungnir::ge::ScreenVertex &v = draw.vertices[vi];
                        if (vi != 0u) frame_out << ", ";
                        frame_out << "[" << v.x << "," << v.y << "," << v.z << "]";
                    }
                    frame_out << "], \"uvs\": [";
                    const std::size_t uv_count = std::min<std::size_t>(draw.vertices.size(), 8u);
                    for (std::size_t vi = 0; vi < uv_count; ++vi) {
                        const gungnir::ge::ScreenVertex &v = draw.vertices[vi];
                        if (vi != 0u) frame_out << ", ";
                        frame_out << "[" << v.texcoord[0] << "," << v.texcoord[1] << "]";
                    }
                    frame_out << "], \"colors\": [";
                    for (std::size_t vi = 0; vi < uv_count; ++vi) {
                        const gungnir::ge::ScreenVertex &v = draw.vertices[vi];
                        if (vi != 0u) frame_out << ", ";
                        frame_out << "[" << v.color[0] << "," << v.color[1] << "," << v.color[2] << ","
                                  << v.color[3] << "]";
                    }
                    frame_out << "]";
                }
                frame_out << "}";
            }
            frame_out << "]\n}\n";
            frame_out.close();
        }

        std::vector<gungnir::ge::Draw> selected_frame = first_frame;
        if (draw_only < selected_frame.size()) {
            gungnir::ge::Draw selected = selected_frame[draw_only];
            selected_frame.assign(1, std::move(selected));
        }
        if (!texture_path.empty() && !selected_frame.empty()) {
            for (const gungnir::ge::Draw &draw : selected_frame) {
                if (draw.state.texture_cpu == nullptr) continue;
                std::vector<std::uint32_t> pixels;
                draw.state.texture_cpu->decode(pixels);
                const int width = draw.state.texture_cpu->state().width;
                const int height = draw.state.texture_cpu->state().height;
                std::size_t alpha_zero = 0u;
                std::size_t alpha_full = 0u;
                for (std::uint32_t value : pixels) {
                    const std::uint32_t alpha = value >> 24u;
                    alpha_zero += alpha == 0u ? 1u : 0u;
                    alpha_full += alpha == 255u ? 1u : 0u;
                }
                std::fprintf(stderr,
                             "texture format=%s swizzled=%d size=%dx%d bufw=%d clut=%08X alpha0=%zu alpha255=%zu\n",
                             gungnir::ge::texture_format_name(draw.state.texture_cpu->state().format),
                             draw.state.texture_cpu->state().swizzled ? 1 : 0, width, height,
                             draw.state.texture_cpu->state().buffer_width,
                             draw.state.texture_cpu->state().clut_format, alpha_zero, alpha_full);
                std::ofstream ppm(texture_path, std::ios::binary);
                if (!ppm) usage("cannot open texture output");
                ppm << "P6\n" << width << " " << height << "\n255\n";
                for (std::uint32_t pixel : pixels) {
                    const char rgb[3] = {static_cast<char>(pixel & 0xFFu),
                                         static_cast<char>((pixel >> 8u) & 0xFFu),
                                         static_cast<char>((pixel >> 16u) & 0xFFu)};
                    ppm.write(rgb, 3);
                }
                break;
            }
        }

        if (!ppm_path.empty() && !selected_frame.empty()) {
            gungnir::ge::Framebuffer framebuffer(480, 272);
            framebuffer.clear(0xFF000000u);
            for (const gungnir::ge::Draw &draw : selected_frame) {
                gungnir::ge::RasterState state;
                state.scissor_x0 = draw.state.scissor_x0;
                state.scissor_y0 = draw.state.scissor_y0;
                state.scissor_x1 = draw.state.scissor_x1;
                state.scissor_y1 = draw.state.scissor_y1;
                state.depth_test = draw.state.depth_test;
                state.depth_write = draw.state.depth_write;
                state.depth_function = draw.state.depth_function;
                state.blend = draw.state.blend;
                state.alpha_test = draw.state.alpha_test;
                state.alpha_function = draw.state.alpha_function;
                state.alpha_reference = draw.state.alpha_reference;
                state.write_mask = draw.state.write_mask;
                state.texture_enabled = draw.state.texture_cpu != nullptr;
                state.texture_enabled = draw.state.texture_cpu != nullptr;
                state.texture = draw.state.texture_cpu;
                gungnir::ge::SoftwareRasterizer rasterizer(framebuffer, state);
                for (std::size_t i = 0; i + 2u < draw.indices.size(); i += 3u) {
                    const auto a = draw.indices[i];
                    const auto b = draw.indices[i + 1u];
                    const auto c = draw.indices[i + 2u];
                    if (a >= draw.vertices.size() || b >= draw.vertices.size() || c >= draw.vertices.size())
                        continue;
                    rasterizer.draw_triangle(draw.vertices[a], draw.vertices[b], draw.vertices[c]);
                }
            }
            std::ofstream ppm(ppm_path, std::ios::binary);
            if (!ppm) usage("cannot open ppm output");
            ppm << "P6\n480 272\n255\n";
            for (int y = 0; y < 272; ++y) {
                for (int x = 0; x < 480; ++x) {
                    const std::uint32_t pixel = framebuffer.color_at(x, y);
                    const char rgb[3] = {static_cast<char>(pixel & 0xFFu),
                                         static_cast<char>((pixel >> 8u) & 0xFFu),
                                         static_cast<char>((pixel >> 16u) & 0xFFu)};
                    ppm.write(rgb, 3);
                }
            }
        }

        if (result.stop_cause == gungnir::session::StopCause::MissingImport) return 1;
        if (!result.reached_first_frame_buf) return 3;
        return 0;
    } catch (const std::exception &failure) {
        std::cerr << "boot_runner: " << failure.what() << "\n";
        return 1;
    }
}
