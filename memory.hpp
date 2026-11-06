#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Broadway {
class Memory {
public:
    explicit Memory(size_t size);
    uint8_t Read8(uint32_t address) const;
    uint16_t Read16(uint32_t address) const;
    uint32_t Read32(uint32_t address) const;
    uint64_t Read64(uint32_t address) const;
    void Write8(uint32_t address, uint8_t value);
    void Write16(uint32_t address, uint16_t value);
    void Write32(uint32_t address, uint32_t value);
    void Write64(uint32_t address, uint64_t value);
    void Clear();
    size_t Size() const;
    const std::vector<uint8_t>& Bytes() const { return data; }
private:
    std::vector<uint8_t> data;
    void CheckBounds(uint32_t address, size_t bytes) const;
};
}
