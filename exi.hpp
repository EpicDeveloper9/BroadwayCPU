#pragma once

#include "hardware_interface.hpp"

#include <array>
#include <cstdint>

namespace Broadway {

class EXIController final : public MemoryMappedDevice {
public:
    static constexpr uint32_t kBaseAddress = 0xCD006800u;
    static constexpr uint32_t kChannelStride = 0x14u;
    static constexpr uint32_t kTransferStart = 1u;

    bool Handles(uint32_t address, unsigned widthBytes) const noexcept override;
    uint32_t Read(uint32_t address, unsigned widthBytes) override;
    void Write(uint32_t address, unsigned widthBytes, uint32_t value) override;
    void Reset() override;

    uint32_t Status(unsigned channel) const;
    uint32_t DmaAddress(unsigned channel) const;
    uint32_t DmaLength(unsigned channel) const;
    uint32_t Control(unsigned channel) const;
    uint32_t ImmediateData(unsigned channel) const;
    uint64_t MmioReads() const noexcept { return mmioReads; }
    uint64_t MmioWrites() const noexcept { return mmioWrites; }
    uint64_t TransferStartRequests() const noexcept { return transferStartRequests; }
    uint64_t TransfersCompleted() const noexcept { return transfersCompleted; }

private:
    struct Channel {
        uint32_t status = 0;
        uint32_t dmaAddress = 0;
        uint32_t dmaLength = 0;
        uint32_t control = 0;
        uint32_t immediateData = 0;
    };

    std::array<Channel, 3> channels{};
    uint64_t mmioReads = 0;
    uint64_t mmioWrites = 0;
    uint64_t transferStartRequests = 0;
    uint64_t transfersCompleted = 0;

    static bool DecodeAddress(uint32_t address, unsigned widthBytes,
                              unsigned& channel, unsigned& reg, unsigned& lane) noexcept;
    static uint32_t RegisterValue(const Channel& channel, unsigned reg) noexcept;
    static void WriteRegister(Channel& channel, unsigned channelIndex,
                              unsigned reg, uint32_t value) noexcept;
};

}
