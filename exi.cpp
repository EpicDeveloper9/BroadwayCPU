#include "exi.hpp"

#include <stdexcept>

namespace Broadway {
namespace {
constexpr uint32_t kWiiUncachedBase = 0x0D006800u;
constexpr uint32_t kCsrChipSelectMask = 7u << 7;
constexpr uint32_t kCsrRomDisable = 1u << 13;
constexpr uint32_t kCsrExternalInterrupt = 1u << 11;
constexpr uint32_t kCsrExternalInterruptMask = 1u << 10;
constexpr uint32_t kCsrTransferComplete = 1u << 3;
constexpr uint32_t kCsrTransferCompleteMask = 1u << 2;
constexpr uint32_t kCsrInterrupt = 1u << 1;
constexpr uint32_t kCsrInterruptMask = 1u;
constexpr uint32_t kCsrClockMask = 7u << 4;
constexpr uint32_t kCsrWritableControl = kCsrClockMask | kCsrChipSelectMask |
    kCsrTransferCompleteMask | kCsrInterruptMask;
constexpr uint32_t kControlTransferStart = 1u;
constexpr uint32_t kControlWritableMask = 0x3Fu;
}

bool EXIController::DecodeAddress(uint32_t address, unsigned widthBytes,
                                  unsigned& channel, unsigned& reg, unsigned& lane) noexcept {
    if (widthBytes != 1 && widthBytes != 2 && widthBytes != 4) return false;
    uint32_t offset;
    if (address >= kBaseAddress && address < kBaseAddress + 0x40u)
        offset = address - kBaseAddress;
    else if (address >= kWiiUncachedBase && address < kWiiUncachedBase + 0x40u)
        offset = address - kWiiUncachedBase;
    else
        return false;

    channel = offset / kChannelStride;
    const unsigned channelOffset = offset % kChannelStride;
    reg = channelOffset / 4;
    lane = channelOffset % 4;
    if (channel >= 3 || reg >= 5 || lane + widthBytes > 4) return false;
    return true;
}

bool EXIController::Handles(uint32_t address, unsigned widthBytes) const noexcept {
    unsigned channel = 0, reg = 0, lane = 0;
    return DecodeAddress(address, widthBytes, channel, reg, lane);
}

uint32_t EXIController::RegisterValue(const Channel& channel, unsigned reg) noexcept {
    switch (reg) {
    case 0: return channel.status;
    case 1: return channel.dmaAddress;
    case 2: return channel.dmaLength;
    case 3: return channel.control;
    case 4: return channel.immediateData;
    default: return 0;
    }
}

void EXIController::WriteRegister(Channel& channel, unsigned channelIndex,
                                  unsigned reg, uint32_t value) noexcept {
    switch (reg) {
    case 0: {
        uint32_t writable = kCsrWritableControl;
        uint32_t clearable = kCsrTransferComplete | kCsrInterrupt;
        if (channelIndex == 0) writable |= kCsrRomDisable;
        if (channelIndex < 2) {
            writable |= kCsrExternalInterruptMask;
            clearable |= kCsrExternalInterrupt;
        }
        const uint32_t writeOneToClear = value & clearable;
        channel.status = (channel.status & ~(writeOneToClear)) | (value & writable);
        break;
    }
    case 1: channel.dmaAddress = value; break;
    case 2: channel.dmaLength = value; break;
    case 3: channel.control = value & kControlWritableMask; break;
    case 4: channel.immediateData = value; break;
    default: break;
    }
}

uint32_t EXIController::Read(uint32_t address, unsigned widthBytes) {
    unsigned channel = 0, reg = 0, lane = 0;
    if (!DecodeAddress(address, widthBytes, channel, reg, lane))
        throw std::out_of_range("EXI MMIO read outside a supported register or width");
    ++mmioReads;
    const uint32_t value = RegisterValue(channels[channel], reg);
    const unsigned shift = (4u - lane - widthBytes) * 8u;
    return (value >> shift) & (widthBytes == 4 ? 0xFFFFFFFFu : ((1u << (widthBytes * 8u)) - 1u));
}

void EXIController::Write(uint32_t address, unsigned widthBytes, uint32_t value) {
    unsigned channelIndex = 0, reg = 0, lane = 0;
    if (!DecodeAddress(address, widthBytes, channelIndex, reg, lane))
        throw std::out_of_range("EXI MMIO write outside a supported register or width");
    ++mmioWrites;
    auto& channel = channels[channelIndex];
    const uint32_t current = RegisterValue(channel, reg);
    const unsigned shift = (4u - lane - widthBytes) * 8u;
    const uint32_t mask = widthBytes == 4 ? 0xFFFFFFFFu : ((1u << (widthBytes * 8u)) - 1u) << shift;
    const uint32_t merged = (current & ~mask) | ((value << shift) & mask);
    WriteRegister(channel, channelIndex, reg, merged);

    if (reg == 3 && (channel.control & kControlTransferStart)) {
        ++transferStartRequests;
        // Transfer timing and EXI devices are not modeled, so completion is unknown.
    }
}

void EXIController::Reset() {
    channels = {};
    mmioReads = 0;
    mmioWrites = 0;
    transferStartRequests = 0;
    transfersCompleted = 0;
}

uint32_t EXIController::Status(unsigned channel) const {
    if (channel >= channels.size()) throw std::out_of_range("EXI channel index out of range");
    return channels[channel].status;
}

uint32_t EXIController::DmaAddress(unsigned channel) const {
    if (channel >= channels.size()) throw std::out_of_range("EXI channel index out of range");
    return channels[channel].dmaAddress;
}

uint32_t EXIController::DmaLength(unsigned channel) const {
    if (channel >= channels.size()) throw std::out_of_range("EXI channel index out of range");
    return channels[channel].dmaLength;
}

uint32_t EXIController::Control(unsigned channel) const {
    if (channel >= channels.size()) throw std::out_of_range("EXI channel index out of range");
    return channels[channel].control;
}

uint32_t EXIController::ImmediateData(unsigned channel) const {
    if (channel >= channels.size()) throw std::out_of_range("EXI channel index out of range");
    return channels[channel].immediateData;
}

}
