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
    constexpr uint32_t kMem1Bytes = 24u * 1024u * 1024u;
    const bool cachedAlias = address >= 0x80000000u && address < 0x81800000u;
    const bool uncachedAlias = address >= 0xC0000000u && address < 0xC1800000u;
    if (cachedAlias && bytes > size_t(0x81800000u - address))
        throw std::out_of_range("Memory access crosses the cached MEM1 alias boundary");
    if (uncachedAlias && bytes > size_t(0xC1800000u - address))
        throw std::out_of_range("Memory access crosses the uncached MEM1 alias boundary");
    const size_t a = TranslateAddress(address);
    if ((cachedAlias || uncachedAlias) && a + bytes > kMem1Bytes)
        throw std::out_of_range("Memory access exceeds emulated MEM1");
    if (a > data.size() || bytes > data.size() - a)
        throw std::out_of_range("Broadway memory access out of bounds");
}
uint32_t Memory::NormalizeMmio(uint32_t address, size_t bytes) const {
    // CC/0C and CD/0D are cached aliases of separate physical MMIO banks.
    uint32_t base = 0;
    uint32_t bank = 0;
    if (address >= 0xCC000000u && address < 0xCC008000u) base = 0xCC000000u;
    else if (address >= 0x0C000000u && address < 0x0C008000u) base = 0x0C000000u;
    else if (address >= 0xCD000000u && address < 0xCD008000u) {
        base = 0xCD000000u;
        bank = 0x8000u;
    } else if (address >= 0x0D000000u && address < 0x0D008000u) {
        base = 0x0D000000u;
        bank = 0x8000u;
    }
    if (base != 0) {
        const uint32_t offset = address - base;
        if (bytes <= size_t(0x8000u - offset)) return bank + offset;
    }
    return 0xFFFFFFFFu;
}
uint8_t Memory::Read8(uint32_t a) const {
    for (auto* device : mmioDevices)
        if (device->Handles(a, 1)) return uint8_t(device->Read(a, 1));
    const uint32_t offset = NormalizeMmio(a,1);
    if (offset != 0xFFFFFFFFu) return mmioStorage[offset];
    CheckBounds(a,1); a = static_cast<uint32_t>(TranslateAddress(a)); return data[a];
}
uint16_t Memory::Read16(uint32_t a) const {
    for (auto* device : mmioDevices)
        if (device->Handles(a, 2)) return uint16_t(device->Read(a, 2));
    const uint32_t offset = NormalizeMmio(a,2);
    if (offset != 0xFFFFFFFFu) return uint16_t((mmioStorage[offset] << 8) | mmioStorage[offset + 1]);
    CheckBounds(a,2); a = static_cast<uint32_t>(TranslateAddress(a));
    return (uint16_t(data[a]) << 8) | uint16_t(data[a+1]);
}
uint32_t Memory::Read32(uint32_t a) const {
    for (auto* device : mmioDevices)
        if (device->Handles(a, 4)) return device->Read(a, 4);
    const uint32_t offset = NormalizeMmio(a,4);
    if (offset != 0xFFFFFFFFu) return (uint32_t(mmioStorage[offset]) << 24) | (uint32_t(mmioStorage[offset + 1]) << 16) |
           (uint32_t(mmioStorage[offset + 2]) << 8) | uint32_t(mmioStorage[offset + 3]);
    CheckBounds(a,4); a = static_cast<uint32_t>(TranslateAddress(a));
    return (uint32_t(data[a])<<24) | (uint32_t(data[a+1])<<16) |
           (uint32_t(data[a+2])<<8) | uint32_t(data[a+3]);
}
uint64_t Memory::Read64(uint32_t a) const {
    for (auto* device : mmioDevices)
        if (device->Handles(a, 8)) throw std::out_of_range("64-bit MMIO access is not supported by this device");
    CheckBounds(a,8);
    return (uint64_t(Read32(a)) << 32) | Read32(a+4);
}
void Memory::Write8(uint32_t a,uint8_t v) {
    for (auto* device : mmioDevices) {
        if (device->Handles(a, 1)) { device->Write(a, 1, v); return; }
    }
    const uint32_t offset = NormalizeMmio(a,1);
    if (offset != 0xFFFFFFFFu) { mmioStorage[offset] = v; return; }
    CheckBounds(a,1); a = static_cast<uint32_t>(TranslateAddress(a)); data[a]=v;
}
void Memory::Write16(uint32_t a,uint16_t v) {
    for (auto* device : mmioDevices) {
        if (device->Handles(a, 2)) { device->Write(a, 2, v); return; }
    }
    const uint32_t offset = NormalizeMmio(a,2);
    if (offset != 0xFFFFFFFFu) { mmioStorage[offset] = uint8_t(v >> 8); mmioStorage[offset + 1] = uint8_t(v); return; }
    CheckBounds(a,2); a = static_cast<uint32_t>(TranslateAddress(a)); data[a]=uint8_t(v>>8); data[a+1]=uint8_t(v);
}
void Memory::Write32(uint32_t a,uint32_t v) {
    for (auto* device : mmioDevices) {
        if (device->Handles(a, 4)) { device->Write(a, 4, v); return; }
    }
    const uint32_t offset = NormalizeMmio(a,4);
    if (offset != 0xFFFFFFFFu) {
        mmioStorage[offset] = uint8_t(v >> 24);
        mmioStorage[offset + 1] = uint8_t(v >> 16);
        mmioStorage[offset + 2] = uint8_t(v >> 8);
        mmioStorage[offset + 3] = uint8_t(v);
        return;
    }
    CheckBounds(a,4); a = static_cast<uint32_t>(TranslateAddress(a)); data[a]=uint8_t(v>>24); data[a+1]=uint8_t(v>>16);
    data[a+2]=uint8_t(v>>8); data[a+3]=uint8_t(v);
}
void Memory::Write64(uint32_t a,uint64_t v) {
    for (auto* device : mmioDevices)
        if (device->Handles(a, 8))
            throw std::out_of_range("64-bit MMIO access is not supported by this device");
    CheckBounds(a,8); Write32(a,uint32_t(v>>32)); Write32(a+4,uint32_t(v));
}
void Memory::Clear() {
    std::fill(data.begin(), data.end(), uint8_t{0});
    std::fill(mmioStorage.begin(), mmioStorage.end(), uint8_t{0});
    for (auto* device : mmioDevices) device->Reset();
}
void Memory::AttachMmioDevice(MemoryMappedDevice* device) {
    mmioDevices.clear();
    if (device) mmioDevices.push_back(device);
}
void Memory::AddMmioDevice(MemoryMappedDevice* device) {
    if (!device) throw std::invalid_argument("Cannot attach a null MMIO device");
    for (const auto* existing : mmioDevices)
        if (existing == device) return;
    mmioDevices.push_back(device);
}
size_t Memory::Size() const { return data.size(); }
}
