#include "memory.hpp"
#include <algorithm>
#include <stdexcept>

namespace Broadway {
namespace {
size_t TranslateAddress(uint32_t address) {
    // Wii MEM1 aliases: cached 0x80000000 and uncached 0xC0000000
    // both refer to physical MEM1 starting at zero.
    if ((address >= 0x80000000u && address <= 0x817FFFFFu) ||
        (address >= 0xC0000000u && address <= 0xC17FFFFFu))
        return static_cast<size_t>(address & 0x01FFFFFFu);
    return static_cast<size_t>(address);
}
}
Memory::Memory(size_t size) : data(size, 0) {}
void Memory::CheckBounds(uint32_t address, size_t bytes) const {
    const size_t a = TranslateAddress(address);
    if (a > data.size() || bytes > data.size() - a)
        throw std::out_of_range("Broadway memory access out of bounds");
}
uint8_t Memory::Read8(uint32_t a) const { CheckBounds(a,1); a = static_cast<uint32_t>(TranslateAddress(a)); return data[a]; }
uint16_t Memory::Read16(uint32_t a) const {
    CheckBounds(a,2); a = static_cast<uint32_t>(TranslateAddress(a));
    return (uint16_t(data[a]) << 8) | uint16_t(data[a+1]);
}
uint32_t Memory::Read32(uint32_t a) const {
    CheckBounds(a,4); a = static_cast<uint32_t>(TranslateAddress(a));
    return (uint32_t(data[a])<<24) | (uint32_t(data[a+1])<<16) |
           (uint32_t(data[a+2])<<8) | uint32_t(data[a+3]);
}
uint64_t Memory::Read64(uint32_t a) const {
    CheckBounds(a,8);
    return (uint64_t(Read32(a)) << 32) | Read32(a+4);
}
void Memory::Write8(uint32_t a,uint8_t v) { CheckBounds(a,1); a = static_cast<uint32_t>(TranslateAddress(a)); data[a]=v; }
void Memory::Write16(uint32_t a,uint16_t v) {
    CheckBounds(a,2); a = static_cast<uint32_t>(TranslateAddress(a)); data[a]=uint8_t(v>>8); data[a+1]=uint8_t(v);
}
void Memory::Write32(uint32_t a,uint32_t v) {
    CheckBounds(a,4); a = static_cast<uint32_t>(TranslateAddress(a)); data[a]=uint8_t(v>>24); data[a+1]=uint8_t(v>>16);
    data[a+2]=uint8_t(v>>8); data[a+3]=uint8_t(v);
}
void Memory::Write64(uint32_t a,uint64_t v) {
    CheckBounds(a,8); Write32(a,uint32_t(v>>32)); Write32(a+4,uint32_t(v));
}
void Memory::Clear() { std::fill(data.begin(),data.end(),0); }
size_t Memory::Size() const { return data.size(); }
}
