#pragma once

#include <cstdint>

namespace Broadway {

// A memory mapped device owns the semantics of its address range. Memory
// delegates only the original access width, preserving big-endian CPU access.
class MemoryMappedDevice {
public:
    virtual ~MemoryMappedDevice() = default;
    virtual bool Handles(uint32_t address, unsigned widthBytes) const noexcept = 0;
    virtual uint32_t Read(uint32_t address, unsigned widthBytes) = 0;
    virtual void Write(uint32_t address, unsigned widthBytes, uint32_t value) = 0;
};

}
