#pragma once

#include <cstdint>

namespace Broadway {

class StarletHardware {
public:
    static constexpr uint32_t HwSrnProt = 0x0D800060u;
    static constexpr uint32_t HwBoot0 = 0x0D80018Cu;
    static constexpr uint32_t BootRomDisabled = 1u << 12;

    StarletHardware() = default;

    void Reset() noexcept;
    uint32_t Read(uint32_t address, unsigned widthBytes) const;
    void Write(uint32_t address, unsigned widthBytes, uint32_t value);

    uint32_t Boot0Register() const noexcept { return boot0; }
    uint32_t SramProtectionRegister() const noexcept { return sramProtection; }

private:
    uint32_t boot0 = 0;
    uint32_t sramProtection = 0;
};

} // namespace Broadway
