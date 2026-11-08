#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace Broadway {

enum class VideoPixelFormat : uint8_t {
    BGRA8,
    RGBA8,
    RGB565BigEndian,
    YUYV422BT601Limited
};

// Immutable, owned frame data. Pitch is in bytes and may exceed the active
// pixel row size. YUYV422 uses Y0,U,Y1,V byte order and requires even width.
class VideoFrame {
public:
    static VideoFrame Create(uint32_t width, uint32_t height, VideoPixelFormat format,
                             size_t rowPitch, std::vector<uint8_t> bytes);

    uint32_t Width() const noexcept { return width; }
    uint32_t Height() const noexcept { return height; }
    size_t RowPitch() const noexcept { return rowPitch; }
    VideoPixelFormat Format() const noexcept { return format; }
    const uint8_t* Data() const noexcept { return storage->data(); }
    size_t DataSize() const noexcept { return storage->size(); }

private:
    VideoFrame(uint32_t w, uint32_t h, VideoPixelFormat f, size_t pitch,
               std::shared_ptr<const std::vector<uint8_t>> data)
        : width(w), height(h), format(f), rowPitch(pitch), storage(std::move(data)) {}

    uint32_t width;
    uint32_t height;
    VideoPixelFormat format;
    size_t rowPitch;
    std::shared_ptr<const std::vector<uint8_t>> storage;
};

struct BGRAFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    size_t rowPitch = 0;
    std::vector<uint8_t> bytes;
};

BGRAFrame ConvertToBGRA8(const VideoFrame& frame);

// One-slot mailbox: producers may publish from another thread; consumers take
// the newest complete frame. Shared ownership keeps a frame alive while the
// renderer uploads it even if another frame arrives concurrently.
class VideoFrameMailbox {
public:
    using Notify = std::function<void()>;

    void Publish(VideoFrame frame);
    std::shared_ptr<const VideoFrame> TakeLatest();
    void SetNotify(Notify notify);
    void Clear();

private:
    std::mutex mutex;
    std::shared_ptr<const VideoFrame> latest;
    Notify notify;
};

}
