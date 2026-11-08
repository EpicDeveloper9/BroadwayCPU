#include "gx.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace Broadway {
namespace {
uint32_t NormalizeMmioAddress(uint32_t address) {
    return address >= 0xC0000000u ? address & 0x3FFFFFFFu : address;
}
uint8_t Clamp(int value) { return uint8_t(std::clamp(value, 0, 255)); }
}

GXProcessor::GXProcessor(Memory& memoryRef, VideoFrameMailbox& mailbox)
    : memory(memoryRef), frames(mailbox), efb(size_t(kEfbWidth) * kEfbHeight * 4, uint8_t{0}) {}

bool GXProcessor::Handles(uint32_t address, unsigned width) const noexcept {
    if (width != 1 && width != 2 && width != 4) return false;
    address = NormalizeMmioAddress(address);
    constexpr uint32_t cpBase = 0x0C000000u, pe = 0x0C001000u, fifo = 0x0C008000u;
    if (address >= cpBase && uint64_t(address) + width <= uint64_t(cpBase) + 0x80) return true;
    if (address >= pe && uint64_t(address) + width <= uint64_t(pe) + 0x100) return true;
    return address >= fifo && uint64_t(address) + width <= uint64_t(fifo) + 4;
}

uint32_t GXProcessor::Read(uint32_t address, unsigned width) {
    if (!Handles(address, width)) throw std::out_of_range("GX MMIO read outside supported register range");
    ++mmioReads;
    address = NormalizeMmioAddress(address);
    constexpr uint32_t cpBase = 0x0C000000u, peBase = 0x0C001000u, fifo = 0x0C008000u;
    if (address >= fifo) {
        SetError("GX write-gather FIFO is write-only.");
        throw std::runtime_error(lastError);
    }
    if (address >= peBase) {
        if (address == peBase + 0x0E && width == 2) return cpToken;
        SetError("Read from an unimplemented GX Pixel Engine register.");
        throw std::runtime_error(lastError);
    }
    const uint32_t offset = address - cpBase;
    if (offset == 0 && width == 2) return uint16_t((stalled ? 0 : 8) | 4);
    if (offset == 2 && width == 2) return cpControl;
    if (offset == 0x0E && width == 2) return cpToken;
    SetError("Read from an unimplemented GX Command Processor register.");
    throw std::runtime_error(lastError);
}

void GXProcessor::Write(uint32_t address, unsigned width, uint32_t value) {
    if (!Handles(address, width)) throw std::out_of_range("GX MMIO write outside supported register range");
    ++mmioWrites;
    address = NormalizeMmioAddress(address);
    constexpr uint32_t cpBase = 0x0C000000u, peBase = 0x0C001000u, fifo = 0x0C008000u;
    if (address >= fifo) {
        for (unsigned i = 0; i < width; ++i)
            FeedCommandByte(uint8_t(value >> ((width - i - 1) * 8)));
        return;
    }
    if (address >= peBase) {
        if (address == peBase + 0x0E && width == 2) cpToken = uint16_t(value);
        else {
            SetError("Write to an unimplemented GX Pixel Engine register.");
            throw std::runtime_error(lastError);
        }
        return;
    }
    const uint32_t offset = address - cpBase;
    if (offset == 2 && width == 2) { cpControl = value; return; }
    if (offset == 4 && width == 2) return;
    if (offset == 0x0E && width == 2) { cpToken = uint16_t(value); return; }
    SetError("Write to an unimplemented GX Command Processor register.");
    throw std::runtime_error(lastError);
}

GXCommandResult GXProcessor::FeedCommandByte(uint8_t byte) {
    if (stalled) return {GXCommandKind::Unsupported, byte, false};
    pending.push_back(byte);
    if (pending.size() == 1) {
        const uint8_t op = pending[0];
        if (op == 0x00 || op == 0x30) expectedCommandBytes = 1;
        else if (op == 0x08) expectedCommandBytes = 6;
        else if (op == 0x10) expectedCommandBytes = 0; // variable length from its header
        else if (op == 0x20) expectedCommandBytes = 5;
        else if (op == 0x28) expectedCommandBytes = 9;
        else if (op == 0x61) expectedCommandBytes = 5;
        else if (op >= 0x80 && op <= 0xBF) {
            SetError("GX primitive command 0x" + std::to_string(op) + " is not rasterized by this GX implementation.");
            stalled = true;
            return {GXCommandKind::Unsupported, op, false};
        } else {
            SetError("Unsupported GX FIFO command opcode.");
            stalled = true;
            return {GXCommandKind::Unsupported, op, false};
        }
    }
    if (pending[0] == 0x10 && pending.size() == 3) {
        const uint16_t header = uint16_t((uint16_t(pending[1]) << 8) | pending[2]);
        const size_t words = size_t((header >> 12) & 0x0f) + 1;
        if (words > (std::numeric_limits<size_t>::max() - 3) / 4) {
            SetError("GX XF command length overflows host size.");
            return {GXCommandKind::Unsupported, 0x10, false};
        }
        expectedCommandBytes = 3 + words * 4;
    }
    GXCommandResult result;
    if (expectedCommandBytes && pending.size() == expectedCommandBytes) {
        if (!ParsePending(result)) return result;
        ++commandsProcessed;
        pending.clear();
        expectedCommandBytes = 0;
        return result;
    }
    return {GXCommandKind::Nop, pending[0], false};
}

bool GXProcessor::ParsePending(GXCommandResult& result) {
    const uint8_t op = pending[0];
    result.opcode = op;
    result.accepted = true;
    if (op == 0x00 || op == 0x30) result.kind = GXCommandKind::Nop;
    else if (op == 0x08) {
        result.kind = GXCommandKind::LoadCP;
        const uint8_t address = pending[1];
        const uint32_t value = (uint32_t(pending[2]) << 24) | (uint32_t(pending[3]) << 16) |
            (uint32_t(pending[4]) << 8) | pending[5];
        cp[address] = value;
    } else if (op == 0x61) {
        result.kind = GXCommandKind::LoadBP;
        const uint32_t word = (uint32_t(pending[1]) << 24) | (uint32_t(pending[2]) << 16) |
            (uint32_t(pending[3]) << 8) | pending[4];
        WriteBP(uint8_t(word >> 24), word & 0x00ffffffu);
    } else if (op == 0x10) {
        result.kind = GXCommandKind::LoadXF;
        const uint16_t header = uint16_t((uint16_t(pending[1]) << 8) | pending[2]);
        const size_t count = size_t((header >> 12) & 0x0f) + 1;
        const uint16_t start = header & 0x0fffu;
        if (size_t(start) + count > xf.size()) {
            SetError("GX XF register load exceeds the documented register address space.");
            result.accepted = false;
            result.kind = GXCommandKind::Unsupported;
            return false;
        }
        for (size_t i = 0; i < count; ++i) {
            const size_t p = 3 + i * 4;
            xf[start + i] = (uint32_t(pending[p]) << 24) | (uint32_t(pending[p + 1]) << 16) |
                (uint32_t(pending[p + 2]) << 8) | pending[p + 3];
        }
    } else if (op == 0x20 || op == 0x28) {
        SetError("GX indexed XF loads and display-list calls are not implemented.");
        result.kind = GXCommandKind::Unsupported;
        result.accepted = false;
        stalled = true;
        return false;
    }
    return true;
}

void GXProcessor::SetError(const std::string& message) { lastError = message; }

void GXProcessor::WriteBP(uint8_t address, uint32_t value) {
    bp[address] = value & 0x00ffffffu;
    if (address == 0x52 && (value & (1u << 14))) CopyEfbToXfb(value);
}

void GXProcessor::SetEfbPixel(uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x >= kEfbWidth || y >= kEfbHeight) throw std::out_of_range("EFB pixel coordinate outside supported surface");
    const size_t offset = (size_t(y) * kEfbWidth + x) * 4;
    efb[offset] = r; efb[offset + 1] = g; efb[offset + 2] = b; efb[offset + 3] = a;
}

void GXProcessor::CopyEfbToXfb(uint32_t control) {
    const uint32_t topLeft = bp[0x49];
    const uint32_t bottomRight = bp[0x4A];
    const uint32_t x0 = topLeft & 0x3ffu, y0 = (topLeft >> 10) & 0x3ffu;
    const uint32_t x1 = bottomRight & 0x3ffu, y1 = (bottomRight >> 10) & 0x3ffu;
    const uint32_t width = x1 >= x0 ? x1 - x0 + 1 : 0;
    const uint32_t sourceHeight = y1 >= y0 ? y1 - y0 + 1 : 0;
    const uint32_t stride = (bp[0x4D] & 0x3ffu) * 16;
    const uint32_t physicalAddress = (bp[0x4B] & 0x00ffffffu) << 5;
    if (!width || !sourceHeight || width > kEfbWidth || stride < width || stride > 1024 ||
        x1 >= kEfbWidth || y1 >= kEfbHeight || (width & 1u)) {
        SetError("GX EFB-to-XFB copy has invalid or unsupported dimensions/stride.");
        return;
    }
    uint32_t outHeight = sourceHeight;
    const uint32_t yScale = bp[0x4E] & 0x1ffu;
    if (yScale) outHeight = std::max(1u, (sourceHeight * 256u) / yScale);
    if (uint64_t(physicalAddress) + uint64_t(stride) * outHeight * 2 > memory.Size()) {
        SetError("GX XFB destination lies outside emulated MEM1.");
        return;
    }
    std::vector<uint8_t> yuyv(size_t(stride) * outHeight * 2, 0);
    for (uint32_t oy = 0; oy < outHeight; ++oy) {
        const uint32_t sy = y0 + std::min(sourceHeight - 1, (oy * sourceHeight) / outHeight);
        for (uint32_t x = 0; x < width; x += 2) {
            const size_t a = (size_t(sy) * kEfbWidth + x0 + x) * 4;
            const size_t b = a + 4;
            const int r0=efb[a], g0=efb[a+1], bl0=efb[a+2];
            const int r1=efb[b], g1=efb[b+1], bl1=efb[b+2];
            const int y0v=((66*r0+129*g0+25*bl0+128)>>8)+16;
            const int y1v=((66*r1+129*g1+25*bl1+128)>>8)+16;
            const int u0=((-38*r0-74*g0+112*bl0+128)>>8)+128;
            const int u1=((-38*r1-74*g1+112*bl1+128)>>8)+128;
            const int v0=((112*r0-94*g0-18*bl0+128)>>8)+128;
            const int v1=((112*r1-94*g1-18*bl1+128)>>8)+128;
            const size_t o=(size_t(oy)*stride+x)*2;
            yuyv[o]=Clamp(y0v); yuyv[o+1]=Clamp((u0+u1+1)/2);
            yuyv[o+2]=Clamp(y1v); yuyv[o+3]=Clamp((v0+v1+1)/2);
        }
    }
    for (size_t i = 0; i < yuyv.size(); ++i) memory.Write8(physicalAddress + uint32_t(i), yuyv[i]);
    frames.Publish(VideoFrame::Create(width, outHeight, VideoPixelFormat::YUYV422BT601Limited,
                                      size_t(stride) * 2, std::move(yuyv)));
    ++framesCopied;
    if (control & (1u << 11)) {
        const uint8_t a = uint8_t(bp[0x4F] >> 8), r = uint8_t(bp[0x4F]);
        const uint8_t g = uint8_t(bp[0x50] >> 8), b = uint8_t(bp[0x50]);
        for (uint32_t y = 0; y < kEfbHeight; ++y)
            for (uint32_t x = 0; x < kEfbWidth; ++x) SetEfbPixel(x,y,r,g,b,a);
    }
}

void GXProcessor::Reset() {
    bp.fill(0); cp.fill(0); xf.fill(0); std::fill(efb.begin(),efb.end(),uint8_t{0});
    pending.clear(); expectedCommandBytes=0; cpControl=0; cpToken=0;
    stalled=false; lastError.clear(); commandsProcessed=0; framesCopied=0;
    mmioReads=0; mmioWrites=0;
}

}
