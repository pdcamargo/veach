// `veach imgdiff`: perceptual image comparison for golden-image tests, using
// NVIDIA FLIP (docs/specs/first-light/spec.md). Mean FLIP error 0 means
// identical; ~0.01 is invisible to most viewers; 1 is maximal.

#include "imgdiff.hpp"

#include <veach/render/png.hpp>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <FLIP.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

/// PNGs are sRGB-encoded; FLIP wants linear RGB in [0, 1].
std::vector<float> to_linear_rgb(const veach::render::Image& image) {
    std::vector<float> out(std::size_t{image.width} * image.height * 3);
    for (std::size_t i = 0, n = std::size_t{image.width} * image.height; i < n; ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            const float v = static_cast<float>(image.rgba[i * 4 + c]) / 255.0f;
            out[i * 3 + c] = v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
        }
    }
    return out;
}

}  // namespace

int command_imgdiff(const std::vector<std::string_view>& args) {
    std::vector<std::string_view> files;
    double max_mean = 0.01;
    std::optional<std::string> diff_path;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--max-mean" && i + 1 < args.size()) {
            const std::string text(args[++i]);
            max_mean = std::stod(text);
        } else if (args[i] == "--diff" && i + 1 < args.size()) {
            diff_path = std::string(args[++i]);
        } else {
            files.push_back(args[i]);
        }
    }
    if (files.size() != 2) {
        std::fprintf(stderr, "usage: veach imgdiff <reference.png> <test.png> [--max-mean 0.01] [--diff diff.png]\n");
        return 2;
    }
    auto reference = veach::render::read_png(std::string(files[0]));
    auto test = veach::render::read_png(std::string(files[1]));
    if (!reference || !test) {
        std::printf("imgdiff: %s\n", (!reference ? reference.error() : test.error()).to_string().c_str());
        return 1;
    }
    if (reference->width != test->width || reference->height != test->height) {
        std::printf("imgdiff: size mismatch: reference %ux%u, test %ux%u\n", reference->width, reference->height,
                    test->width, test->height);
        return 1;
    }

    const std::vector<float> ref_linear = to_linear_rgb(*reference);
    const std::vector<float> test_linear = to_linear_rgb(*test);
    FLIP::Parameters parameters;
    float mean = 0.0f;
    float* error_map = nullptr;
    FLIP::evaluate(ref_linear.data(), test_linear.data(), static_cast<int>(reference->width),
                   static_cast<int>(reference->height), false, parameters, false, true, mean, &error_map);
    const std::unique_ptr<float[]> errors(error_map);
    const std::size_t pixels = std::size_t{reference->width} * reference->height;
    float worst = 0.0f;
    std::size_t above = 0;
    for (std::size_t i = 0; i < pixels; ++i) {
        worst = std::max(worst, errors[i]);
        above += errors[i] > 0.1f ? 1 : 0;
    }
    const bool pass = mean <= max_mean;
    std::printf("imgdiff: mean FLIP %.5f (limit %.5f), worst pixel %.3f, %.3f%% of pixels above 0.1, %ux%u: %s\n",
                static_cast<double>(mean), max_mean, static_cast<double>(worst),
                100.0 * static_cast<double>(above) / static_cast<double>(pixels), reference->width, reference->height,
                pass ? "PASS" : "FAIL");

    if (diff_path && !pass) {
        float* magma = nullptr;
        float unused = 0.0f;
        FLIP::evaluate(ref_linear.data(), test_linear.data(), static_cast<int>(reference->width),
                       static_cast<int>(reference->height), false, parameters, true, false, unused, &magma);
        const std::unique_ptr<float[]> map(magma);
        std::vector<std::uint8_t> rgba(pixels * 4);
        for (std::size_t i = 0; i < pixels; ++i) {
            for (std::size_t c = 0; c < 3; ++c) {
                rgba[i * 4 + c] = static_cast<std::uint8_t>(std::clamp(map[i * 3 + c], 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            rgba[i * 4 + 3] = 255;
        }
        if (veach::render::write_png(*diff_path, reference->width, reference->height, rgba)) {
            std::printf("imgdiff: error map written to %s\n", diff_path->c_str());
        }
    }
    return pass ? 0 : 1;
}
