#include <veach/render/png.hpp>

#include <format>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>
#include <stb_image_write.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace veach::render {

Result<> write_png(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                   std::span<const std::uint8_t> rgba) {
    if (rgba.size() < std::size_t{width} * height * 4) {
        return make_error(ErrorCode::internal, "write_png: not enough pixel data");
    }
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    const std::string file = path.string();
    if (stbi_write_png(file.c_str(), static_cast<int>(width), static_cast<int>(height), 4, rgba.data(),
                       static_cast<int>(width * 4)) == 0) {
        return make_error(ErrorCode::io_failed, std::format("cannot write PNG '{}'", path.generic_string()));
    }
    return {};
}

Result<Image> read_png(const std::filesystem::path& path) {
    int w = 0;
    int h = 0;
    int channels = 0;
    const std::string file = path.string();
    stbi_uc* pixels = stbi_load(file.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        return make_error(ErrorCode::io_failed,
                          std::format("cannot read PNG '{}': {}", path.generic_string(), stbi_failure_reason()));
    }
    Image image{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), {}};
    image.rgba.assign(pixels, pixels + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    stbi_image_free(pixels);
    return image;
}

}  // namespace veach::render
