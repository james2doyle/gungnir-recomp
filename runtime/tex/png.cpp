#include "tex/png.hpp"

#include <csetjmp>
#include <climits>
#include <cstdio>
#include <limits>
#include <vector>

#include <png.h>

namespace gungnir::tex {
namespace {

struct PngError {
    std::jmp_buf jump;
};

void png_error_callback(png_structp png, png_const_charp) {
    auto *error = static_cast<PngError *>(png_get_error_ptr(png));
    if (error != nullptr) std::longjmp(error->jump, 1);
    std::longjmp(png_jmpbuf(png), 1);
}

void png_warning_callback(png_structp, png_const_charp) {}

} // namespace

PngReadResult read_png(const std::filesystem::path &path) {
    PngReadResult result;
    FILE *file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        result.error = "file_not_found";
        return result;
    }

    PngError error;
    png_structp png =
        png_create_read_struct(PNG_LIBPNG_VER_STRING, &error, png_error_callback,
                               png_warning_callback);
    if (png == nullptr) {
        std::fclose(file);
        result.error = "png_read_failed";
        return result;
    }
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        std::fclose(file);
        result.error = "png_read_failed";
        return result;
    }

    const auto cleanup = [&]() {
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(file);
    };

    if (setjmp(error.jump)) {
        cleanup();
        result.error = "png_read_failed";
        return result;
    }

    png_init_io(png, file);
    png_read_info(png, info);

    png_uint_32 width = 0;
    png_uint_32 height = 0;
    int bit_depth = 0;
    int color_type = 0;
    int interlace = 0;
    png_get_IHDR(png, info, &width, &height, &bit_depth, &color_type, &interlace, nullptr,
                 nullptr);
    if (width == 0 || height == 0 || width > static_cast<png_uint_32>(INT_MAX) ||
        height > static_cast<png_uint_32>(INT_MAX)) {
        cleanup();
        result.error = "png_unsupported_layout";
        return result;
    }

    png_set_expand(png);
    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    const bool has_alpha = (color_type & PNG_COLOR_MASK_ALPHA) != 0 ||
                           png_get_valid(png, info, PNG_INFO_tRNS) != 0;
    if (!has_alpha) png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);

    const png_size_t row_bytes = png_get_rowbytes(png, info);
    if (row_bytes != static_cast<png_size_t>(width) * 4u ||
        static_cast<std::size_t>(height) >
            std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(row_bytes)) {
        cleanup();
        result.error = "png_unsupported_layout";
        return result;
    }

    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(height) *
                                   static_cast<std::size_t>(row_bytes));
    std::vector<png_bytep> rows(static_cast<std::size_t>(height));
    for (std::size_t y = 0; y < rows.size(); ++y)
        rows[y] = rgba.data() + y * static_cast<std::size_t>(row_bytes);
    png_read_image(png, rows.data());
    png_read_end(png, nullptr);
    cleanup();

    result.image.width = static_cast<int>(width);
    result.image.height = static_cast<int>(height);
    result.image.rgba = std::move(rgba);
    result.ok = true;
    return result;
}

PngWriteResult write_png(const std::filesystem::path &path, int width, int height,
                         std::span<const std::uint8_t> rgba) {
    PngWriteResult result;
    if (width <= 0 || height <= 0 ||
        rgba.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u) {
        result.error = "invalid_image";
        return result;
    }

    FILE *file = std::fopen(path.string().c_str(), "wb");
    if (file == nullptr) {
        result.error = "file_open_failed";
        return result;
    }

    PngError error;
    png_structp png =
        png_create_write_struct(PNG_LIBPNG_VER_STRING, &error, png_error_callback,
                                png_warning_callback);
    if (png == nullptr) {
        std::fclose(file);
        result.error = "png_write_failed";
        return result;
    }
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_write_struct(&png, nullptr);
        std::fclose(file);
        result.error = "png_write_failed";
        return result;
    }

    const auto cleanup = [&]() {
        png_destroy_write_struct(&png, &info);
        std::fclose(file);
    };

    if (setjmp(error.jump)) {
        cleanup();
        result.error = "png_write_failed";
        return result;
    }

    png_init_io(png, file);
    png_set_IHDR(png, info, static_cast<png_uint_32>(width),
                 static_cast<png_uint_32>(height), 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_compression_level(png, 6);
    png_write_info(png, info);

    std::vector<png_bytep> rows(static_cast<std::size_t>(height));
    for (std::size_t y = 0; y < rows.size(); ++y)
        rows[y] = const_cast<png_bytep>(rgba.data() +
                                        y * static_cast<std::size_t>(width) * 4u);
    png_write_image(png, rows.data());
    png_write_end(png, nullptr);
    cleanup();

    result.ok = true;
    return result;
}

} // namespace gungnir::tex
