// Spike SP-06 (VCH-082), throwaway: the numbers and images of docs/research/spikes/sp-06-field-graph.md.
//   veach-bench-sp06 [--resolution 1024] [--frames 50] [--repeats 5] [--out docs/research/spikes/sp-06]
// CPU samples/s on one thread (batch and scalar), GPU ms per pass from the render graph's timings (interpreter and
// hand-written), and the largest differences between the three. Build with the release preset for the timings.

#include "sp06/field_gpu.hpp"

#include <veach/render/png.hpp>
#include <veach/rhi/rhi.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace veach;

namespace {

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

double median(std::vector<double> v) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

struct Difference {
    double max_abs = 0.0;
    double mean_abs = 0.0;
};

Difference difference(const std::vector<float>& a, const std::vector<float>& b) {
    Difference d;
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double e = std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
        d.max_abs = std::max(d.max_abs, e);
        sum += e;
    }
    d.mean_abs = a.empty() ? 0.0 : sum / static_cast<double>(a.size());
    return d;
}

std::uint8_t to_byte(double v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
}

/// Panels side by side, each 2x2 box-filtered to half resolution: heights normalized to [lo, hi], then
/// |interpreter - CPU| with white at `error_white`.
void write_panels(const std::filesystem::path& path, std::uint32_t res,
                  const std::vector<const std::vector<float>*>& heights, const std::vector<float>& cpu,
                  const std::vector<float>& interpreted, double error_white) {
    const auto [lo_it, hi_it] = std::minmax_element(cpu.begin(), cpu.end());
    const double lo = *lo_it;
    const double hi = *hi_it;
    const std::uint32_t half = res / 2;
    const auto panels = static_cast<std::uint32_t>(heights.size() + 1);
    const std::uint32_t gap = 8;
    const std::uint32_t width = panels * half + (panels - 1) * gap;
    std::vector<std::uint8_t> rgba(std::size_t{width} * half * 4, 255);
    auto box = [&](const std::vector<float>& v, std::uint32_t x, std::uint32_t y) {
        const std::size_t i = std::size_t{2 * y} * res + 2 * x;
        return 0.25 * (static_cast<double>(v[i]) + v[i + 1] + v[i + res] + v[i + res + 1]);
    };
    for (std::uint32_t y = 0; y < half; ++y) {
        for (std::uint32_t x = 0; x < half; ++x) {
            for (std::uint32_t p = 0; p < panels; ++p) {
                std::uint8_t r = 0;
                std::uint8_t g = 0;
                std::uint8_t b = 0;
                if (p < heights.size()) {
                    r = g = b = to_byte((box(*heights[p], x, y) - lo) / (hi - lo));
                } else {
                    double e = 0.0;
                    for (std::uint32_t k = 0; k < 4; ++k) {
                        const std::size_t i = std::size_t{2 * y + k / 2} * res + 2 * x + k % 2;
                        e = std::max(e, std::abs(static_cast<double>(interpreted[i]) - cpu[i]));
                    }
                    r = to_byte(e / error_white);
                    g = to_byte(e / error_white * 0.5);
                    b = 0;
                }
                const std::size_t o = (std::size_t{y} * width + p * (half + gap) + x) * 4;
                rgba[o] = r;
                rgba[o + 1] = g;
                rgba[o + 2] = b;
            }
        }
    }
    if (auto ok = render::write_png(path, width, half, rgba); !ok) {
        std::fprintf(stderr, "sp06: %s\n", ok.error().message.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::uint32_t res = 1024;
    std::uint32_t frames = 50;
    int repeats = 5;
    std::filesystem::path out;
    std::filesystem::path graph;  // another graph than terrain.field.json (diagnostics; hand-written stays terrain)
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view flag = argv[i];
        const char* value = argv[i + 1];
        if (flag == "--resolution") {
            res = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
        } else if (flag == "--frames") {
            frames = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
        } else if (flag == "--repeats") {
            repeats = std::atoi(value);
        } else if (flag == "--out") {
            out = value;
        } else if (flag == "--graph") {
            graph = value;
        }
    }

    auto program = sp06::terrain_program();
    if (!graph.empty()) {
        std::ifstream file(graph, std::ios::binary);
        const std::string json(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>{});
        auto parsed = procgen::parse_field_graph(json, graph.string());
        program = parsed ? procgen::compile(*parsed, {.seed = 42, .radius_planet_m = 6.0e6})
                         : Result<procgen::FieldProgram>(std::unexpected(parsed.error()));
    }
    if (!program) {
        std::fprintf(stderr, "%s\n", program.error().message.c_str());
        return 1;
    }
    procgen::FaceRegion region;
    region.resolution = res;
    const std::size_t samples = std::size_t{res} * res;
    std::printf("graph: %zu op-list words, %u registers, %u noise octaves per sample; %u x %u samples\n",
                program->words.size(), program->registers, program->noise_octaves, res, res);

    // ---- CPU, one thread
    std::vector<float> cpu(samples);
    std::vector<float> scalar(samples);
    double best_batch = 1e30;
    for (int r = 0; r < repeats; ++r) {
        const auto start = std::chrono::steady_clock::now();
        procgen::evaluate(*program, region, cpu);
        best_batch = std::min(best_batch, seconds_since(start));
    }
    double best_scalar = 1e30;
    for (int r = 0; r < std::max(1, repeats / 2); ++r) {
        const auto start = std::chrono::steady_clock::now();
        procgen::evaluate(*program, region, scalar, {.batch = 1});
        best_scalar = std::min(best_scalar, seconds_since(start));
    }
    const bool bit_equal = std::memcmp(cpu.data(), scalar.data(), samples * sizeof(float)) == 0;
    // One octave of noise alone, for the cost per noise evaluation.
    std::vector<float> noise(samples);
    double best_noise = 1e30;
    for (int r = 0; r < repeats; ++r) {
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < samples; ++i) {
            const float x = static_cast<float>(i % res) * 0.031f;
            const float y = static_cast<float>(i / res) * 0.029f;
            noise[i] = procgen::gradient_noise(x, y, 0.37f, 3.0f, 0x9E3779B9u);
        }
        best_noise = std::min(best_noise, seconds_since(start));
    }
    const double batch_rate = static_cast<double>(samples) / best_batch;
    const double scalar_rate = static_cast<double>(samples) / best_scalar;
    std::printf(
        "| CPU batch interpreter (%zu per batch) | %.2f M samples/s | %.1f ns/sample | %.2f ns per noise octave |\n",
        std::size_t{procgen::max_batch}, batch_rate * 1e-6, 1e9 / batch_rate,
        1e9 / batch_rate / program->noise_octaves);
    std::printf("| CPU scalar interpreter (batch of 1) | %.2f M samples/s | %.1f ns/sample | |\n", scalar_rate * 1e-6,
                1e9 / scalar_rate);
    std::printf("| one noise octave, plain loop | %.1f M evaluations/s | %.2f ns | checksum %.3f |\n",
                static_cast<double>(samples) / best_noise * 1e-6, best_noise * 1e9 / static_cast<double>(samples),
                static_cast<double>(noise[samples / 3]));
    std::printf("batch == scalar bit for bit: %s\n", bit_equal ? "yes" : "NO");

    // ---- GPU
#if defined(__APPLE__)
    constexpr auto backend = rhi::Backend::metal;
#elif defined(_WIN32)
    constexpr auto backend = rhi::Backend::dx12;
#else
    constexpr auto backend = rhi::Backend::null;
#endif
    auto device = rhi::create_device({.backend = backend, .validation = false});
    if (!device || !(*device)->capabilities().gpu) {
        std::printf("no GPU: %s\n", device ? "null device" : device.error().message.c_str());
        return bit_equal ? 0 : 1;
    }
    std::printf("GPU: %s (%s)\n", (*device)->capabilities().device_name.c_str(),
                std::string(rhi::to_string(backend)).c_str());
    auto gpu = sp06::run_gpu(**device, *program, region, frames);
    if (!gpu) {
        std::fprintf(stderr, "%s\n", gpu.error().message.c_str());
        return 1;
    }
    // The first frames include pipeline warm-up: drop them.
    const auto steady = [](std::vector<double> v) {
        v.erase(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(3, v.size() / 2)));
        return v;
    };
    const std::vector<double> interp_ms = steady(gpu->interpreted_ms);
    const std::vector<double> hand_ms = steady(gpu->handwritten_ms);
    if (!gpu->timing_unavailable.empty()) {
        std::printf("GPU timings unavailable: %s\n", gpu->timing_unavailable.c_str());
    } else {
        const double mi = median(interp_ms);
        const double mh = median(hand_ms);
        const double per_million = 1e6 / static_cast<double>(samples);
        std::printf(
            "| GPU interpreter | median %.4f ms | min %.4f ms | %.4f ms per 10^6 samples | %.0f M samples/s |\n", mi,
            *std::min_element(interp_ms.begin(), interp_ms.end()), mi * per_million,
            static_cast<double>(samples) / mi * 1e-3);
        std::printf(
            "| GPU hand-written | median %.4f ms | min %.4f ms | %.4f ms per 10^6 samples | %.0f M samples/s |\n", mh,
            *std::min_element(hand_ms.begin(), hand_ms.end()), mh * per_million,
            static_cast<double>(samples) / mh * 1e-3);
        std::printf("interpreter / hand-written: %.2fx (%zu frames)\n", mi / mh, interp_ms.size());
    }
    const Difference di = difference(gpu->interpreted, cpu);
    const Difference dh = difference(gpu->handwritten, cpu);
    const Difference dg = difference(gpu->interpreted, gpu->handwritten);
    const auto [lo, hi] = std::minmax_element(cpu.begin(), cpu.end());
    std::printf("CPU range [%.6f, %.6f]\n", static_cast<double>(*lo), static_cast<double>(*hi));
    std::printf("| GPU interpreter vs CPU | max %.3g | mean %.3g |\n", di.max_abs, di.mean_abs);
    std::printf("| hand-written vs CPU | max %.3g | mean %.3g |\n", dh.max_abs, dh.mean_abs);
    std::printf("| GPU interpreter vs hand-written | max %.3g | mean %.3g |\n", dg.max_abs, dg.mean_abs);

    if (!out.empty()) {
        std::filesystem::create_directories(out);
        write_panels(out / "side_by_side.png", res, {&cpu, &gpu->interpreted, &gpu->handwritten}, cpu, gpu->interpreted,
                     1e-6);  // the error panel is white at 1e-6, a tenth of the tolerance
        std::printf("wrote %s\n", (out / "side_by_side.png").string().c_str());
    }
    const bool agree = di.max_abs < 1e-5 && dh.max_abs < 1e-5;
    return bit_equal && agree ? 0 : 1;
}
