#include "video_frame.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Broadway {
namespace {
size_t BytesPerPixel(VideoPixelFormat format) {
    switch (format) {
    case VideoPixelFormat::BGRA8:
    case VideoPixelFormat::RGBA8: return 4;
    case VideoPixelFormat::RGB565BigEndian:
    case VideoPixelFormat::YUYV422BT601Limited: return 2;
    }
    throw std::invalid_argument("Unknown video pixel format.");
}

uint8_t ClampByte(int value) {
    return uint8_t(std::clamp(value, 0, 255));
}
}

VideoFrame VideoFrame::Create(uint32_t width, uint32_t height, VideoPixelFormat format,
                              size_t rowPitch, std::vector<uint8_t> bytes) {
    if (width == 0 || height == 0)
        throw std::invalid_argument("Video frame dimensions must be nonzero.");
    if (format == VideoPixelFormat::YUYV422BT601Limited && (width & 1u))
        throw std::invalid_argument("YUYV422 frames require an even pixel width.");
    const size_t bpp = BytesPerPixel(format);
    if (size_t(width) > std::numeric_limits<size_t>::max() / bpp)
        throw std::invalid_argument("Video frame row size overflows addressable memory.");
    const size_t minimumPitch = size_t(width) * bpp;
    if (rowPitch < minimumPitch)
        throw std::invalid_argument("Video frame row pitch is smaller than its active pixel row.");
    if (size_t(height) > std::numeric_limits<size_t>::max() / rowPitch)
        throw std::invalid_argument("Video frame storage size overflows addressable memory.");
    const size_t requiredBytes = rowPitch * size_t(height);
    if (bytes.size() < requiredBytes)
        throw std::invalid_argument("Video frame storage is shorter than pitch multiplied by height.");
    auto owned = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    return VideoFrame(width, height, format, rowPitch, std::move(owned));
}

BGRAFrame ConvertToBGRA8(const VideoFrame& frame) {
    const uint32_t width = frame.Width();
    const uint32_t height = frame.Height();
    if (size_t(width) > std::numeric_limits<size_t>::max() / 4 ||
        size_t(height) > std::numeric_limits<size_t>::max() / (size_t(width) * 4))
        throw std::length_error("Converted video frame is too large.");
    BGRAFrame output{width, height, size_t(width) * 4,
                     std::vector<uint8_t>(size_t(width) * height * 4)};
    const uint8_t* src = frame.Data();
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = src + size_t(y) * frame.RowPitch();
        uint8_t* dst = output.bytes.data() + size_t(y) * output.rowPitch;
        for (uint32_t x = 0; x < width; ++x) {
            switch (frame.Format()) {
            case VideoPixelFormat::BGRA8:
                std::copy_n(row + size_t(x) * 4, 4, dst + size_t(x) * 4);
                break;
            case VideoPixelFormat::RGBA8:
                dst[size_t(x) * 4] = row[size_t(x) * 4 + 2];
                dst[size_t(x) * 4 + 1] = row[size_t(x) * 4 + 1];
                dst[size_t(x) * 4 + 2] = row[size_t(x) * 4];
                dst[size_t(x) * 4 + 3] = row[size_t(x) * 4 + 3];
                break;
            case VideoPixelFormat::RGB565BigEndian: {
                const uint16_t packed = uint16_t((uint16_t(row[size_t(x) * 2]) << 8) |
                                                 row[size_t(x) * 2 + 1]);
                const uint8_t r5 = uint8_t((packed >> 11) & 31u);
                const uint8_t g6 = uint8_t((packed >> 5) & 63u);
                const uint8_t b5 = uint8_t(packed & 31u);
                dst[size_t(x) * 4] = uint8_t((b5 << 3) | (b5 >> 2));
                dst[size_t(x) * 4 + 1] = uint8_t((g6 << 2) | (g6 >> 4));
                dst[size_t(x) * 4 + 2] = uint8_t((r5 << 3) | (r5 >> 2));
                dst[size_t(x) * 4 + 3] = 255;
                break;
            }
            case VideoPixelFormat::YUYV422BT601Limited: {
                const size_t group = size_t(x & ~1u) * 2;
                const int yy = int(row[group + ((x & 1u) ? 2 : 0)]) - 16;
                const int u = int(row[group + 1]) - 128;
                const int v = int(row[group + 3]) - 128;
                const int c = std::max(0, yy);
                dst[size_t(x) * 4] = ClampByte((298 * c + 516 * u + 128) >> 8);
                dst[size_t(x) * 4 + 1] = ClampByte((298 * c - 100 * u - 208 * v + 128) >> 8);
                dst[size_t(x) * 4 + 2] = ClampByte((298 * c + 409 * v + 128) >> 8);
                dst[size_t(x) * 4 + 3] = 255;
                break;
            }
            }
        }
    }
    return output;
}

void VideoFrameMailbox::Publish(VideoFrame frame) {
    auto owned = std::make_shared<const VideoFrame>(std::move(frame));
    Notify wake;
    {
        std::lock_guard lock(mutex);
        latest = std::move(owned);
        wake = notify;
    }
    if (wake) wake();
}

std::shared_ptr<const VideoFrame> VideoFrameMailbox::TakeLatest() {
    std::lock_guard lock(mutex);
    return std::exchange(latest, {});
}

void VideoFrameMailbox::SetNotify(Notify callback) {
    std::lock_guard lock(mutex);
    notify = std::move(callback);
}

void VideoFrameMailbox::Clear() {
    std::lock_guard lock(mutex);
    latest.reset();
}

}
