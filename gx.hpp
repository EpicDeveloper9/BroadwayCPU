#pragma once

#include "hardware_interface.hpp"
#include "memory.hpp"
#include "video_frame.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Broadway {

enum class GXCommandKind : uint8_t { Nop, LoadCP, LoadXF, LoadBP, Unsupported };

struct GXCommandResult {
    GXCommandKind kind = GXCommandKind::Nop;
    uint8_t opcode = 0;
    bool accepted = false;
};

// Broadway's memory-mapped GX command ingress plus a software model of the
// EFB-to-XFB display-copy path. The rasterizer is intentionally not implied:
// unsupported geometry commands stop with a diagnostic instead of drawing a
// guessed result.
class GXProcessor final : public MemoryMappedDevice {
public:
    static constexpr uint32_t kEfbWidth = 640;
    static constexpr uint32_t kEfbHeight = 528;

    GXProcessor(Memory& memory, VideoFrameMailbox& frames);
    bool Handles(uint32_t address, unsigned widthBytes) const noexcept override;
    uint32_t Read(uint32_t address, unsigned widthBytes) override;
    void Write(uint32_t address, unsigned widthBytes, uint32_t value) override;

    GXCommandResult FeedCommandByte(uint8_t byte);
    void SetEfbPixel(uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
    const std::string& LastError() const noexcept { return lastError; }
    uint64_t CommandsProcessed() const noexcept { return commandsProcessed; }
    uint64_t FramesCopied() const noexcept { return framesCopied; }
    uint64_t MmioReads() const noexcept { return mmioReads; }
    uint64_t MmioWrites() const noexcept { return mmioWrites; }
    bool IsStalled() const noexcept { return stalled; }
    void Reset();

private:
    Memory& memory;
    VideoFrameMailbox& frames;
    std::array<uint32_t, 256> bp{};
    std::array<uint32_t, 256> cp{};
    std::array<uint32_t, 4096> xf{};
    std::vector<uint8_t> efb;
    std::vector<uint8_t> pending;
    size_t expectedCommandBytes = 0;
    uint32_t cpControl = 0;
    uint16_t cpToken = 0;
    bool stalled = false;
    std::string lastError;
    uint64_t commandsProcessed = 0;
    uint64_t framesCopied = 0;
    uint64_t mmioReads = 0;
    uint64_t mmioWrites = 0;

    bool ParsePending(GXCommandResult& result);
    void SetError(const std::string& message);
    void WriteBP(uint8_t address, uint32_t value);
    void CopyEfbToXfb(uint32_t control);
};

}
