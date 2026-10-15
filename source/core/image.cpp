// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "core/image.hpp"

#include <png.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

namespace tz
{

namespace
{

// The largest PNG decoded, per side and in all. Memory isn't the limit (big images are scaled down a row at a time)
// but time is: 64 megapixels take several seconds to inflate on a 3DS.
constexpr uint32_t kMaxSide = 16384;
constexpr uint64_t kMaxPixels = uint64_t{8192} * 8192;
// Interlaced PNGs arrive in seven passes over the whole image, so they are decoded whole before being scaled.
constexpr uint64_t kMaxInterlacedPixels = uint64_t{2048} * 2048;
// The largest image handed back.
constexpr uint64_t kMaxResultPixels = uint64_t{2048} * 2048;

uint8_t toByte(float value)
{
    return static_cast<uint8_t>(std::clamp(value + 0.5f, 0.0f, 255.0f));
}

// Shrinks an image arriving a row at a time. Every target pixel averages the part of the source it covers, with
// fractional weights for source pixels on its edges, weighted by alpha so that the colour under transparent pixels
// (often black) doesn't darken the edges of what is next to them. Holds two target rows of sums, whatever the size of
// the source.
class AreaScaler
{
public:
    AreaScaler(int sourceWidth, int sourceHeight, int width, int height)
        : sourceHeight_(sourceHeight), result_(width, height), columns_(static_cast<size_t>(sourceWidth)),
          rows_(static_cast<size_t>(sourceHeight)), line_(static_cast<size_t>(width) * 4),
          sums_{std::vector<float>(static_cast<size_t>(width) * 4), std::vector<float>(static_cast<size_t>(width) * 4)}
    {
        cover(columns_, sourceWidth, width);
        cover(rows_, sourceHeight, height);
    }

    void addRow(const uint8_t* rgba)
    {
        const Span& row = rows_[static_cast<size_t>(nextRow_++)];
        while (finished_ < row.first)
            finishRow();

        std::fill(line_.begin(), line_.end(), 0.0f);
        for (size_t x = 0; x < columns_.size(); ++x, rgba += 4)
        {
            const float a = rgba[3];
            if (a == 0.0f)
                continue;
            const float pixel[4] = {rgba[0] * a, rgba[1] * a, rgba[2] * a, a};
            const Span& column = columns_[x];
            float* first = &line_[static_cast<size_t>(column.first) * 4];
            for (int c = 0; c < 4; ++c)
                first[c] += pixel[c] * column.firstWeight;
            if (column.secondWeight > 0.0f)
                for (int c = 0; c < 4; ++c)
                    first[4 + c] += pixel[c] * column.secondWeight;
        }

        accumulate(row.first, row.firstWeight);
        if (row.secondWeight > 0.0f)
            accumulate(row.first + 1, row.secondWeight);
        if (nextRow_ == sourceHeight_)
            while (finished_ < result_.height)
                finishRow();
    }

    Image take() { return std::move(result_); }

private:
    // Which target pixels (one, or two neighbours) a source pixel lands in, and how much of it each gets.
    struct Span
    {
        int first = 0;
        float firstWeight = 0;
        float secondWeight = 0;
    };

    // In units where a source pixel is `target` long and a target pixel `source` long, both are whole numbers: the
    // overlaps are exact, and each target pixel's weights add up to one.
    static void cover(std::vector<Span>& spans, int source, int target)
    {
        const int64_t s = source;
        const int64_t t = target;
        for (int64_t i = 0; i < s; ++i)
        {
            const int64_t start = i * t;
            const int64_t end = start + t;
            Span& span = spans[static_cast<size_t>(i)];
            span.first = static_cast<int>(start / s);
            const int64_t boundary = (span.first + 1) * s;
            if (end <= boundary)
            {
                span.firstWeight = static_cast<float>(t) / static_cast<float>(s);
            }
            else
            {
                span.firstWeight = static_cast<float>(boundary - start) / static_cast<float>(s);
                span.secondWeight = static_cast<float>(end - boundary) / static_cast<float>(s);
            }
        }
    }

    void accumulate(int targetRow, float weight)
    {
        std::vector<float>& sums = sums_[targetRow & 1];
        for (size_t i = 0; i < sums.size(); ++i)
            sums[i] += line_[i] * weight;
    }

    void finishRow()
    {
        std::vector<float>& sums = sums_[finished_ & 1];
        uint8_t* out = result_.at(0, finished_);
        for (size_t i = 0; i < sums.size(); i += 4, out += 4)
        {
            const float alpha = sums[i + 3];
            if (alpha >= 0.5f)
            {
                out[0] = toByte(sums[i] / alpha);
                out[1] = toByte(sums[i + 1] / alpha);
                out[2] = toByte(sums[i + 2] / alpha);
                out[3] = toByte(alpha);
            }
            else
            {
                out[0] = out[1] = out[2] = out[3] = 0;
            }
        }
        std::fill(sums.begin(), sums.end(), 0.0f);
        ++finished_;
    }

    int sourceHeight_;
    int nextRow_ = 0;
    int finished_ = 0;
    Image result_;
    std::vector<Span> columns_;
    std::vector<Span> rows_;
    std::vector<float> line_;
    std::vector<float> sums_[2];
};

Image shrink(const Image& image, int width, int height)
{
    AreaScaler scaler(image.width, image.height, width, height);
    for (int y = 0; y < image.height; ++y)
        scaler.addRow(image.at(0, y));
    return scaler.take();
}

// Bilinear, between pixel centres, on alpha-weighted colours.
Image enlarge(const Image& image, int width, int height)
{
    struct Sample
    {
        int low;
        int high;
        float fraction;
    };
    auto samples = [](int source, int target) {
        std::vector<Sample> result(static_cast<size_t>(target));
        for (int i = 0; i < target; ++i)
        {
            const float position = std::clamp((static_cast<float>(i) + 0.5f) * static_cast<float>(source) /
                                                      static_cast<float>(target) -
                                                  0.5f,
                                              0.0f, static_cast<float>(source - 1));
            const int low = static_cast<int>(position);
            result[static_cast<size_t>(i)] = {low, std::min(low + 1, source - 1), position - static_cast<float>(low)};
        }
        return result;
    };
    const std::vector<Sample> xs = samples(image.width, width);
    const std::vector<Sample> ys = samples(image.height, height);

    Image result(width, height);
    for (int y = 0; y < height; ++y)
    {
        const Sample& sy = ys[static_cast<size_t>(y)];
        for (int x = 0; x < width; ++x)
        {
            const Sample& sx = xs[static_cast<size_t>(x)];
            const uint8_t* corners[4] = {image.at(sx.low, sy.low), image.at(sx.high, sy.low), image.at(sx.low, sy.high),
                                         image.at(sx.high, sy.high)};
            const float weights[4] = {(1 - sx.fraction) * (1 - sy.fraction), sx.fraction * (1 - sy.fraction),
                                      (1 - sx.fraction) * sy.fraction, sx.fraction * sy.fraction};
            float sums[4] = {};
            for (int i = 0; i < 4; ++i)
            {
                const float a = corners[i][3] * weights[i];
                sums[0] += corners[i][0] * a;
                sums[1] += corners[i][1] * a;
                sums[2] += corners[i][2] * a;
                sums[3] += a;
            }
            uint8_t* out = result.at(x, y);
            if (sums[3] >= 0.5f)
            {
                out[0] = toByte(sums[0] / sums[3]);
                out[1] = toByte(sums[1] / sums[3]);
                out[2] = toByte(sums[2] / sums[3]);
                out[3] = toByte(sums[3]);
            }
        }
    }
    return result;
}

// ---- libpng
//
// libpng reports errors by longjmp()ing back to a setjmp() in the function that called it. Destructors of C++
// objects in the frames it jumps over don't run, so everything that owns memory lives in a job struct of the caller,
// and the frames between setjmp and libpng only hold plain pointers and numbers.

struct PngJob
{
    png_structp png = nullptr;
    png_infop info = nullptr;
    Errc code = Errc::InvalidData;
    char message[96] = {};
};

[[noreturn]] void onPngError(png_structp png, png_const_charp message)
{
    auto* job = static_cast<PngJob*>(png_get_error_ptr(png));
    std::snprintf(job->message, sizeof(job->message), "%s", message ? message : "PNG error");
    png_longjmp(png, 1);
}

void onPngWarning(png_structp, png_const_charp)
{
}

// Fails the decode with this error code and message.
[[noreturn]] void stop(PngJob& job, Errc code, const char* message)
{
    job.code = code;
    png_error(job.png, message);
}

struct DecodeJob : PngJob
{
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t offset = 0;
    int maxWidth = 0;
    int maxHeight = 0;

    Image image;               // the result
    Image whole;               // an interlaced image before it is scaled
    Bytes row;                 // one row, to be scaled
    std::vector<png_bytep> rows; // where libpng puts each row of an interlaced image
    std::optional<AreaScaler> scaler;
};

void readFromMemory(png_structp png, png_bytep out, size_t length)
{
    auto* job = static_cast<DecodeJob*>(png_get_io_ptr(png));
    if (length > job->size - job->offset)
        png_error(png, "PNG data ends early");
    std::memcpy(out, job->data + job->offset, length);
    job->offset += length;
}

// The largest size within the limits with the image's aspect ratio, never larger than the image.
void fit(uint32_t width, uint32_t height, uint32_t maxWidth, uint32_t maxHeight, uint32_t& outWidth,
         uint32_t& outHeight)
{
    outWidth = width;
    outHeight = height;
    if (width <= maxWidth && height <= maxHeight)
        return;
    if (uint64_t{width} * maxHeight > uint64_t{height} * maxWidth)
    {
        outWidth = maxWidth;
        outHeight = static_cast<uint32_t>((uint64_t{height} * maxWidth + width / 2) / width);
    }
    else
    {
        outHeight = maxHeight;
        outWidth = static_cast<uint32_t>((uint64_t{width} * maxHeight + height / 2) / height);
    }
    outWidth = std::clamp<uint32_t>(outWidth, 1, width);
    outHeight = std::clamp<uint32_t>(outHeight, 1, height);
}

// Everything that can make libpng jump: the job and the png pointers live in the caller's frame.
void decodeSteps(DecodeJob& job)
{
    png_structp png = job.png;
    png_infop info = job.info;
    png_set_read_fn(png, &job, readFromMemory);
    png_set_user_limits(png, kMaxSide, kMaxSide);
    png_read_info(png, info);

    const uint32_t width = png_get_image_width(png, info);
    const uint32_t height = png_get_image_height(png, info);
    const png_byte colorType = png_get_color_type(png, info);
    const png_byte bitDepth = png_get_bit_depth(png, info);
    const bool interlaced = png_get_interlace_type(png, info) != PNG_INTERLACE_NONE;
    const bool transparency = png_get_valid(png, info, PNG_INFO_tRNS) != 0;
    if (uint64_t{width} * height > kMaxPixels)
        stop(job, Errc::TooLarge, "PNG too large");

    // to 8-bit RGBA, whatever it is
    if (colorType == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (transparency)
        png_set_tRNS_to_alpha(png);
    if (bitDepth == 16)
        png_set_scale_16(png);
    if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    if (!(colorType & PNG_COLOR_MASK_ALPHA) && !transparency)
        png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    if (interlaced)
        png_set_interlace_handling(png);
    png_read_update_info(png, info);
    if (png_get_channels(png, info) != 4 || png_get_bit_depth(png, info) != 8 ||
        png_get_rowbytes(png, info) != size_t{width} * 4)
        stop(job, Errc::Unsupported, "unexpected PNG layout");

    uint32_t targetWidth;
    uint32_t targetHeight;
    fit(width, height, static_cast<uint32_t>(job.maxWidth), static_cast<uint32_t>(job.maxHeight), targetWidth,
        targetHeight);
    if (uint64_t{targetWidth} * targetHeight > kMaxResultPixels)
        stop(job, Errc::TooLarge, "image too large");
    const bool scaled = targetWidth != width || targetHeight != height;

    if (interlaced)
    {
        if (uint64_t{width} * height > kMaxInterlacedPixels)
            stop(job, Errc::TooLarge, "interlaced PNG too large");
        Image& target = scaled ? job.whole : job.image;
        target = Image(static_cast<int>(width), static_cast<int>(height));
        job.rows.resize(height);
        for (uint32_t y = 0; y < height; ++y)
            job.rows[y] = target.at(0, static_cast<int>(y));
        png_read_image(png, job.rows.data());
        // scaled by the caller, outside of libpng's reach
        return;
    }

    if (!scaled)
    {
        job.image = Image(static_cast<int>(width), static_cast<int>(height));
        for (uint32_t y = 0; y < height; ++y)
            png_read_row(png, job.image.at(0, static_cast<int>(y)), nullptr);
        return;
    }

    job.scaler.emplace(static_cast<int>(width), static_cast<int>(height), static_cast<int>(targetWidth),
                       static_cast<int>(targetHeight));
    job.row.resize(size_t{width} * 4);
    for (uint32_t y = 0; y < height; ++y)
    {
        png_read_row(png, job.row.data(), nullptr);
        job.scaler->addRow(job.row.data());
    }
    job.image = job.scaler->take();
}

bool runDecode(DecodeJob& job)
{
    job.png = png_create_read_struct(PNG_LIBPNG_VER_STRING, static_cast<PngJob*>(&job), onPngError, onPngWarning);
    if (job.png)
        job.info = png_create_info_struct(job.png);
    if (!job.png || !job.info)
    {
        png_destroy_read_struct(&job.png, &job.info, nullptr);
        job.code = Errc::OutOfMemory;
        return false;
    }
    if (setjmp(png_jmpbuf(job.png)))
    {
        png_destroy_read_struct(&job.png, &job.info, nullptr);
        return false;
    }
    decodeSteps(job);
    png_destroy_read_struct(&job.png, &job.info, nullptr);
    return true;
}

struct EncodeJob : PngJob
{
    const Image* image = nullptr;
    Bytes out;
};

void writeToMemory(png_structp png, png_bytep data, size_t length)
{
    auto* job = static_cast<EncodeJob*>(png_get_io_ptr(png));
    job->out.insert(job->out.end(), data, data + length);
}

void flushNothing(png_structp)
{
}

void encodeSteps(EncodeJob& job)
{
    png_structp png = job.png;
    const Image& image = *job.image;
    png_set_write_fn(png, &job, writeToMemory, flushNothing);

    // opaque images are written without their alpha, which makes them smaller
    bool opaque = true;
    for (size_t i = 3; i < image.pixels.size() && opaque; i += 4)
        opaque = image.pixels[i] == 0xFF;
    png_set_IHDR(png, job.info, static_cast<png_uint_32>(image.width), static_cast<png_uint_32>(image.height), 8,
                 opaque ? PNG_COLOR_TYPE_RGB : PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, job.info);
    if (opaque)
        png_set_filler(png, 0, PNG_FILLER_AFTER); // the rows still have a fourth byte, to be dropped
    for (int y = 0; y < image.height; ++y)
        png_write_row(png, image.at(0, y));
    png_write_end(png, nullptr);
}

bool runEncode(EncodeJob& job)
{
    job.png = png_create_write_struct(PNG_LIBPNG_VER_STRING, static_cast<PngJob*>(&job), onPngError, onPngWarning);
    if (job.png)
        job.info = png_create_info_struct(job.png);
    if (!job.png || !job.info)
    {
        png_destroy_write_struct(&job.png, &job.info);
        job.code = Errc::OutOfMemory;
        return false;
    }
    if (setjmp(png_jmpbuf(job.png)))
    {
        png_destroy_write_struct(&job.png, &job.info);
        return false;
    }
    encodeSteps(job);
    png_destroy_write_struct(&job.png, &job.info);
    return true;
}

bool validImage(const Image& image)
{
    return !image.empty() && image.pixels.size() == size_t(image.width) * size_t(image.height) * 4;
}

} // namespace

Expected<Image> decodePng(ByteSpan png, int maxWidth, int maxHeight)
{
    if (png.size() < 8 || png_sig_cmp(png.data(), 0, 8) != 0)
        return fail(Errc::InvalidData, "not a PNG");

    DecodeJob job;
    job.data = png.data();
    job.size = png.size();
    job.maxWidth = std::max(maxWidth, 1);
    job.maxHeight = std::max(maxHeight, 1);
    if (!runDecode(job))
        return fail(job.code, job.message);

    if (!job.whole.empty())
    {
        uint32_t width;
        uint32_t height;
        fit(static_cast<uint32_t>(job.whole.width), static_cast<uint32_t>(job.whole.height),
            static_cast<uint32_t>(job.maxWidth), static_cast<uint32_t>(job.maxHeight), width, height);
        return shrink(job.whole, static_cast<int>(width), static_cast<int>(height));
    }
    return std::move(job.image);
}

Expected<Bytes> encodePng(const Image& image)
{
    if (!validImage(image))
        return fail(Errc::InvalidData, "empty image");
    EncodeJob job;
    job.image = &image;
    if (!runEncode(job))
        return fail(job.code, job.message);
    return std::move(job.out);
}

Image resize(const Image& image, int width, int height)
{
    if (width <= 0 || height <= 0)
        return Image();
    if (!validImage(image))
        return Image(width, height);
    if (width == image.width && height == image.height)
        return image;
    if (width <= image.width && height <= image.height)
        return shrink(image, width, height);
    if (width >= image.width && height >= image.height)
        return enlarge(image, width, height);
    // smaller one way and larger the other: average the way it shrinks, then interpolate the way it grows
    return enlarge(shrink(image, std::min(width, image.width), std::min(height, image.height)), width, height);
}

Image crop(const Image& image, int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0)
        return Image();
    Image result(width, height);
    if (!validImage(image))
        return result;
    const int left = std::max(x, 0);
    const int right = std::min(x + width, image.width);
    const int top = std::max(y, 0);
    const int bottom = std::min(y + height, image.height);
    if (left >= right || top >= bottom)
        return result;
    for (int row = top; row < bottom; ++row)
        std::memcpy(result.at(left - x, row - y), image.at(left, row), static_cast<size_t>(right - left) * 4);
    return result;
}

void tileRgb565(const Image& image, uint16_t* texture, uint32_t textureWidth, uint32_t textureHeight, int x, int y)
{
    for (int row = 0; row < image.height; ++row)
    {
        const int ty = y + row;
        if (ty < 0 || static_cast<uint32_t>(ty) >= textureHeight)
            continue;
        for (int column = 0; column < image.width; ++column)
        {
            const int tx = x + column;
            if (tx < 0 || static_cast<uint32_t>(tx) >= textureWidth)
                continue;
            const uint8_t* pixel = image.at(column, row);
            texture[tiledIndex(static_cast<uint32_t>(tx), static_cast<uint32_t>(ty), textureWidth)] =
                toRgb565(pixel[0], pixel[1], pixel[2]);
        }
    }
}

void tileRgba8(const Image& image, uint32_t* texture, uint32_t textureWidth, uint32_t textureHeight, int x, int y)
{
    for (int row = 0; row < image.height; ++row)
    {
        const int ty = y + row;
        if (ty < 0 || static_cast<uint32_t>(ty) >= textureHeight)
            continue;
        for (int column = 0; column < image.width; ++column)
        {
            const int tx = x + column;
            if (tx < 0 || static_cast<uint32_t>(tx) >= textureWidth)
                continue;
            const uint8_t* pixel = image.at(column, row);
            texture[tiledIndex(static_cast<uint32_t>(tx), static_cast<uint32_t>(ty), textureWidth)] =
                static_cast<uint32_t>(pixel[0]) << 24 | static_cast<uint32_t>(pixel[1]) << 16 |
                static_cast<uint32_t>(pixel[2]) << 8 | pixel[3];
        }
    }
}

Image iconToImage(const uint16_t* tiled, int size)
{
    Image image(size, size);
    for (int y = 0; y < size; ++y)
    {
        for (int x = 0; x < size; ++x)
        {
            const uint16_t value =
                tiled[tiledIndex(static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(size))];
            const uint8_t r = (value >> 11) & 0x1F;
            const uint8_t g = (value >> 5) & 0x3F;
            const uint8_t b = value & 0x1F;
            uint8_t* pixel = image.at(x, y);
            // widen by repeating the high bits, so that full intensity stays 255
            pixel[0] = static_cast<uint8_t>(r << 3 | r >> 2);
            pixel[1] = static_cast<uint8_t>(g << 2 | g >> 4);
            pixel[2] = static_cast<uint8_t>(b << 3 | b >> 2);
            pixel[3] = 0xFF;
        }
    }
    return image;
}

void imageToIcon(const Image& image, uint16_t* tiled, int size)
{
    const Image fitted = image.width == size && image.height == size ? image : resize(image, size, size);
    for (int y = 0; y < size; ++y)
    {
        for (int x = 0; x < size; ++x)
        {
            // icons have no alpha: what is transparent goes black, as it shows in the HOME Menu without any
            const uint8_t* pixel = fitted.at(x, y);
            const unsigned a = pixel[3];
            tiled[tiledIndex(static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(size))] =
                toRgb565(static_cast<uint8_t>(pixel[0] * a / 255), static_cast<uint8_t>(pixel[1] * a / 255),
                         static_cast<uint8_t>(pixel[2] * a / 255));
        }
    }
}

} // namespace tz
