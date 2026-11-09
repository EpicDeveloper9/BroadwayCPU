#include "gx.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
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
    return address >= fifo && uint64_t(address) + width <= uint64_t(fifo) + 0x20;
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
        if (address == peBase + 0x00 && width == 2) return peZConfig;
        if (address == peBase + 0x0E && width == 2) return cpToken;
        if (address == peBase + 0x02 && width == 2) return peAlphaConfig;
        if (address == peBase + 0x04 && width == 2) return peDestinationAlpha;
        if (address == peBase + 0x06 && width == 2) return peAlphaMode;
        if (address == peBase + 0x08 && width == 2) return peAlphaRead;
        if (address == peBase + 0x0A && width == 2) return peStatus;
        SetError("Read from an unimplemented GX Pixel Engine register.");
        throw std::runtime_error(lastError);
    }
    const uint32_t offset = address - cpBase;
    if (offset == 0 && width == 2) return uint16_t((stalled ? 0 : 8) | 4);
    if (offset == 2 && width == 2) return cpControl;
    if (offset == 6 && width == 2) return cpPerfSelect;
    if (offset == 0x0E && width == 2) return cpToken;
    if (offset >= 0x20 && offset < 0x40 && (offset & 1u) == 0 && width == 2)
        return cpFifoRegisters[(offset - 0x20) / 2];
    SetError("Read from an unimplemented GX Command Processor register.");
    throw std::runtime_error(lastError);
}

void GXProcessor::Write(uint32_t address, unsigned width, uint32_t value) {
    if (!Handles(address, width)) throw std::out_of_range("GX MMIO write outside supported register range");
    ++mmioWrites;
    address = NormalizeMmioAddress(address);
    constexpr uint32_t cpBase = 0x0C000000u, peBase = 0x0C001000u, fifo = 0x0C008000u;
    if (address >= fifo) {
        for (unsigned i = 0; i < width; ++i) {
            const uint8_t byte = uint8_t(value >> ((width - i - 1) * 8));
            if (fifoTrace.size() < 256) fifoTrace.push_back(byte);
            FeedCommandByte(byte);
        }
        return;
    }
    if (address >= peBase) {
        if (address == peBase + 0x00 && width == 2) peZConfig = uint16_t(value);
        else if (address == peBase + 0x0E && width == 2) cpToken = uint16_t(value);
        else if (address == peBase + 0x02 && width == 2) peAlphaConfig = uint16_t(value);
        else if (address == peBase + 0x04 && width == 2) peDestinationAlpha = uint16_t(value);
        else if (address == peBase + 0x06 && width == 2) peAlphaMode = uint16_t(value);
        else if (address == peBase + 0x08 && width == 2) peAlphaRead = uint16_t(value);
        else if (address == peBase + 0x0A && width == 2)
            peStatus &= uint16_t(~(value & 0x0F));
        else {
            SetError("Write to an unimplemented GX Pixel Engine register.");
            throw std::runtime_error(lastError);
        }
        return;
    }
    const uint32_t offset = address - cpBase;
    if (offset == 2 && width == 2) { cpControl = value; return; }
    if (offset == 4 && width == 2) return;
    if (offset == 6 && width == 2) { cpPerfSelect = uint16_t(value & 0x0007u); return; }
    if (offset == 0x0E && width == 2) { cpToken = uint16_t(value); return; }
    if (offset >= 0x20 && offset < 0x40 && (offset & 1u) == 0 && width == 2) {
        cpFifoRegisters[(offset - 0x20) / 2] = uint16_t(value);
        return;
    }
    SetError("Write to an unimplemented GX Command Processor register.");
    throw std::runtime_error(lastError);
}

GXCommandResult GXProcessor::FeedCommandByte(uint8_t byte) {
    if (stalled) return {GXCommandKind::Unsupported, byte, false};
    pending.push_back(byte);
    if (pending.size() == 1) {
        const uint8_t op = pending[0];
        if (op == 0x00 || op == 0x30 || op == 0x48) expectedCommandBytes = 1;
        else if (op == 0x08) expectedCommandBytes = 6;
        else if (op == 0x10) expectedCommandBytes = 0; // variable length from its header
        else if (op == 0x20) expectedCommandBytes = 5;
        else if (op == 0x28) expectedCommandBytes = 9;
        else if (op == 0x61) expectedCommandBytes = 5;
        else if (op >= 0x80 && op <= 0xBF) {
            expectedCommandBytes = 3;
        } else {
            SetError("Unsupported GX FIFO command opcode " + std::to_string(op) + ".");
            stalled = true;
            return {GXCommandKind::Unsupported, op, false};
        }
    }
    if (pending[0] == 0x10 && pending.size() == 5) {
        const uint32_t header = (uint32_t(pending[1]) << 24) |
            (uint32_t(pending[2]) << 16) | (uint32_t(pending[3]) << 8) | pending[4];
        const size_t words = size_t(header >> 16) + 1;
        if (words > (std::numeric_limits<size_t>::max() - 5) / 4) {
            SetError("GX XF command length overflows host size.");
            return {GXCommandKind::Unsupported, 0x10, false};
        }
        expectedCommandBytes = 5 + words * 4;
    }
    if (pending.size() == 3 && pending[0] >= 0x80 && pending[0] <= 0xBF) {
        const uint16_t vertexCount = uint16_t((uint16_t(pending[1]) << 8) | pending[2]);
        const size_t vertexSize = PrimitiveVertexSize(pending[0]);
        if (!vertexSize) {
            stalled = true;
            return {GXCommandKind::Unsupported, pending[0], false};
        }
        if (vertexCount % 4 != 0 ||
            vertexCount > (std::numeric_limits<size_t>::max() - 3) / vertexSize) {
            SetError("GX quad command has an invalid vertex count.");
            stalled = true;
            return {GXCommandKind::Unsupported, pending[0], false};
        }
        expectedCommandBytes = 3 + size_t(vertexCount) * vertexSize;
    }
    GXCommandResult result;
    if (expectedCommandBytes && pending.size() == expectedCommandBytes) {
        if (!ParsePending(result)) {
            stalled = true;
            return result;
        }
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
    else if (op == 0x48) result.kind = GXCommandKind::InvalidateVertexCache;
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
        const uint32_t header = (uint32_t(pending[1]) << 24) |
            (uint32_t(pending[2]) << 16) | (uint32_t(pending[3]) << 8) | pending[4];
        const size_t count = size_t(header >> 16) + 1;
        const uint32_t start = header & 0xffffu;
        if (size_t(start) + count > xf.size()) {
            SetError("GX XF register load exceeds the documented register address space.");
            result.accepted = false;
            result.kind = GXCommandKind::Unsupported;
            return false;
        }
        for (size_t i = 0; i < count; ++i) {
            const size_t p = 5 + i * 4;
            xf[start + i] = (uint32_t(pending[p]) << 24) | (uint32_t(pending[p + 1]) << 16) |
                (uint32_t(pending[p + 2]) << 8) | pending[p + 3];
        }
    } else if (op >= 0x80 && op <= 0xBF) {
        result.kind = GXCommandKind::Primitive;
        if (!RenderPrimitive()) {
            result.accepted = false;
            result.kind = GXCommandKind::Unsupported;
            return false;
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

size_t GXProcessor::PrimitiveVertexSize(uint8_t opcode) {
    if (((opcode >> 3) & 0x0Fu) != 0) {
        SetError("Only GX_QUADS primitives are supported by the current rasterizer.");
        return 0;
    }

    const uint8_t vat = opcode & 0x07u;
    const uint32_t vcdLow = cp[0x50];
    const uint32_t vcdHigh = cp[0x60];
    const uint32_t vatA = cp[0x70u + vat];
    const bool directPosition = ((vcdLow >> 9) & 3u) == 1;
    const bool directColor0 = ((vcdLow >> 13) & 3u) == 1;
    const uint32_t usedVatAFields = 0x0001FFFFu;
    if (vcdLow != 0x00002200u || vcdHigh != 0 || !directPosition ||
        !directColor0 || (vatA & usedVatAFields) != 0x00016009u) {
        SetError("GX primitive uses an unsupported vertex descriptor or VAT format.");
        return 0;
    }
    return 16;
}

bool GXProcessor::RenderPrimitive() {
    const uint16_t vertexCount = uint16_t((uint16_t(pending[1]) << 8) | pending[2]);
    struct Vertex {
        float x;
        float y;
        float z;
        double screenX;
        double screenY;
        std::array<uint8_t, 4> color;
    };

    auto ReadWord = [this](size_t offset) {
        return (uint32_t(pending[offset]) << 24) |
               (uint32_t(pending[offset + 1]) << 16) |
               (uint32_t(pending[offset + 2]) << 8) |
               uint32_t(pending[offset + 3]);
    };
    auto XfFloat = [this](size_t index) {
        return std::bit_cast<float>(xf[index]);
    };

    const float projectionA = XfFloat(0x1020);
    const float projectionB = XfFloat(0x1021);
    const float projectionC = XfFloat(0x1022);
    const float projectionD = XfFloat(0x1023);
    const float projectionE = XfFloat(0x1024);
    const float projectionF = XfFloat(0x1025);
    const bool orthographic = (xf[0x1026] & 1u) != 0;
    const double viewportScaleX = XfFloat(0x101A);
    const double viewportScaleY = XfFloat(0x101B);
    const double viewportOffsetX = XfFloat(0x101D) - 342.0;
    const double viewportOffsetY = XfFloat(0x101E) - 342.0;

    std::vector<Vertex> vertices;
    vertices.reserve(vertexCount);
    size_t cursor = 3;
    for (uint16_t i = 0; i < vertexCount; ++i) {
        Vertex vertex{};
        vertex.x = std::bit_cast<float>(ReadWord(cursor));
        vertex.y = std::bit_cast<float>(ReadWord(cursor + 4));
        vertex.z = std::bit_cast<float>(ReadWord(cursor + 8));
        for (size_t channel = 0; channel < vertex.color.size(); ++channel)
            vertex.color[channel] = pending[cursor + 12 + channel];
        cursor += 16;

        const double x = vertex.x * std::bit_cast<float>(xf[0]) +
                         vertex.y * std::bit_cast<float>(xf[1]) +
                         vertex.z * std::bit_cast<float>(xf[2]) +
                         std::bit_cast<float>(xf[3]);
        const double y = vertex.x * std::bit_cast<float>(xf[4]) +
                         vertex.y * std::bit_cast<float>(xf[5]) +
                         vertex.z * std::bit_cast<float>(xf[6]) +
                         std::bit_cast<float>(xf[7]);
        const double z = vertex.x * std::bit_cast<float>(xf[8]) +
                         vertex.y * std::bit_cast<float>(xf[9]) +
                         vertex.z * std::bit_cast<float>(xf[10]) +
                         std::bit_cast<float>(xf[11]);
        const double clipX = projectionA * x + (orthographic ? projectionB : projectionB * z);
        const double clipY = projectionC * y + (orthographic ? projectionD : projectionD * z);
        const double clipZ = projectionE * z + projectionF;
        const double clipW = orthographic ? 1.0 : -z;
        if (clipW == 0.0) {
            SetError("GX primitive vertex projects to zero homogeneous W.");
            return false;
        }
        const double ndcX = clipX / clipW;
        const double ndcY = clipY / clipW;
        vertex.screenX = ndcX * viewportScaleX + viewportOffsetX;
        vertex.screenY = ndcY * viewportScaleY + viewportOffsetY;
        if (!std::isfinite(vertex.screenX) || !std::isfinite(vertex.screenY) ||
            !std::isfinite(clipZ)) {
            SetError("GX primitive produced non-finite transformed coordinates.");
            return false;
        }
        vertices.push_back(vertex);
    }

    const uint32_t scissorLeftRaw = (bp[0x20] >> 12) & 0x3FFu;
    const uint32_t scissorTopRaw = bp[0x20] & 0x3FFu;
    const uint32_t scissorRightRaw = (bp[0x21] >> 12) & 0x3FFu;
    const uint32_t scissorBottomRaw = bp[0x21] & 0x3FFu;
    const bool scissorConfigured = bp[0x20] != 0 || bp[0x21] != 0;
    const int minX = scissorConfigured ? std::max(0, int(scissorLeftRaw) - 342) : 0;
    const int minY = scissorConfigured ? std::max(0, int(scissorTopRaw) - 342) : 0;
    const int maxX = scissorConfigured
        ? std::min(int(kEfbWidth - 1), int(scissorRightRaw) - 342)
        : int(kEfbWidth - 1);
    const int maxY = scissorConfigured
        ? std::min(int(kEfbHeight - 1), int(scissorBottomRaw) - 342)
        : int(kEfbHeight - 1);

    auto RasterizeTriangle = [this, minX, minY, maxX, maxY](const Vertex& a,
                                                            const Vertex& b,
                                                            const Vertex& c) {
        const double area = (b.screenX - a.screenX) * (c.screenY - a.screenY) -
                            (b.screenY - a.screenY) * (c.screenX - a.screenX);
        if (area == 0.0) return;
        const int left = std::max(minX, int(std::floor(std::min({a.screenX, b.screenX, c.screenX}))));
        const int right = std::min(maxX, int(std::ceil(std::max({a.screenX, b.screenX, c.screenX}))));
        const int top = std::max(minY, int(std::floor(std::min({a.screenY, b.screenY, c.screenY}))));
        const int bottom = std::min(maxY, int(std::ceil(std::max({a.screenY, b.screenY, c.screenY}))));
        for (int y = top; y <= bottom; ++y) {
            for (int x = left; x <= right; ++x) {
                const double px = x + 0.5;
                const double py = y + 0.5;
                const double w0 = ((b.screenX - px) * (c.screenY - py) -
                                   (b.screenY - py) * (c.screenX - px)) / area;
                const double w1 = ((c.screenX - px) * (a.screenY - py) -
                                   (c.screenY - py) * (a.screenX - px)) / area;
                const double w2 = 1.0 - w0 - w1;
                if (w0 < -1e-9 || w1 < -1e-9 || w2 < -1e-9) continue;
                std::array<uint8_t, 4> color{};
                for (size_t channel = 0; channel < color.size(); ++channel) {
                    const double value = w0 * a.color[channel] + w1 * b.color[channel] +
                                         w2 * c.color[channel];
                    color[channel] = Clamp(int(std::lround(value)));
                }
                SetEfbPixel(uint32_t(x), uint32_t(y), color[0], color[1], color[2], color[3]);
                ++efbPixelsWritten;
            }
        }
    };

    for (size_t i = 0; i < vertices.size(); i += 4) {
        RasterizeTriangle(vertices[i], vertices[i + 1], vertices[i + 2]);
        RasterizeTriangle(vertices[i], vertices[i + 2], vertices[i + 3]);
        ++primitivesSubmitted;
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
    cpFifoRegisters.fill(0);
    pending.clear(); fifoTrace.clear(); expectedCommandBytes=0;
    cpControl=0; cpPerfSelect=0; cpToken=0;
    peZConfig=0; peAlphaConfig=0; peDestinationAlpha=0;
    peAlphaMode=0; peAlphaRead=0; peStatus=0;
    stalled=false; lastError.clear(); commandsProcessed=0; framesCopied=0;
    primitivesSubmitted=0; efbPixelsWritten=0;
    mmioReads=0; mmioWrites=0;
}

}
