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

enum class GXCommandKind : uint8_t { Nop, InvalidateVertexCache, LoadCP, LoadXF, LoadBP, Primitive, Unsupported };

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
    uint64_t PrimitivesSubmitted() const noexcept { return primitivesSubmitted; }
    uint64_t EfbPixelsWritten() const noexcept { return efbPixelsWritten; }
    uint32_t CpRegisterValue(uint8_t address) const noexcept { return cp[address]; }
    uint32_t XfRegisterValue(uint16_t address) const noexcept { return xf[address]; }
    uint32_t BpRegisterValue(uint8_t address) const noexcept { return bp[address]; }
    size_t PendingFifoBytes() const noexcept { return pending.size(); }
    size_t ExpectedFifoBytes() const noexcept { return expectedCommandBytes; }
    uint64_t MmioReads() const noexcept { return mmioReads; }
    uint64_t MmioWrites() const noexcept { return mmioWrites; }
    const std::vector<uint8_t>& FifoTrace() const noexcept { return fifoTrace; }
    bool IsStalled() const noexcept { return stalled; }
    void Reset();

private:
    Memory& memory;
    VideoFrameMailbox& frames;
    std::array<uint32_t, 256> bp{};
    std::array<uint32_t, 256> cp{};
    std::array<uint16_t, 16> cpFifoRegisters{};
    std::array<uint32_t, 0x1100> xf{};
    std::vector<uint8_t> efb;
    std::vector<uint8_t> pending;
    std::vector<uint8_t> fifoTrace;
    size_t expectedCommandBytes = 0;
    uint32_t cpControl = 0;
    uint16_t cpPerfSelect = 0;
    uint16_t cpToken = 0;
    uint16_t peZConfig = 0;
    uint16_t peAlphaConfig = 0;
    uint16_t peDestinationAlpha = 0;
    uint16_t peAlphaMode = 0;
    uint16_t peAlphaRead = 0;
    uint16_t peStatus = 0;
    bool stalled = false;
    std::string lastError;
    uint64_t commandsProcessed = 0;
    uint64_t framesCopied = 0;
    uint64_t primitivesSubmitted = 0;
    uint64_t efbPixelsWritten = 0;
    uint64_t mmioReads = 0;
    uint64_t mmioWrites = 0;

    bool ParsePending(GXCommandResult& result);
    size_t PrimitiveVertexSize(uint8_t opcode);
    bool RenderPrimitive();
    void SetError(const std::string& message);
    void WriteBP(uint8_t address, uint32_t value);
    void CopyEfbToXfb(uint32_t control);
};

}
