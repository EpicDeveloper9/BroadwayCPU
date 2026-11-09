#include "starlet_hardware.hpp"

#include <sstream>
#include <stdexcept>

namespace Broadway {
namespace {
std::runtime_error UnsupportedAccess(uint32_t address, unsigned widthBytes, bool write) {
    std::ostringstream message;
    message << "Unsupported Starlet " << (write ? "write" : "read")
            << " width " << widthBytes << " at 0x" << std::hex << address;
    return std::runtime_error(message.str());
}
}

void StarletHardware::Reset() noexcept {
    // Reset leaves the boot ROM visible; other control bits start clear.
    boot0 = 0;
    sramProtection = 0;
}

uint32_t StarletHardware::Read(uint32_t address, unsigned widthBytes) const {
    if (widthBytes != 4 || (address & 3u) != 0)
        throw UnsupportedAccess(address, widthBytes, false);
    switch (address) {
        case HwBoot0: return boot0;
        case HwSrnProt: return sramProtection;
        default: throw UnsupportedAccess(address, widthBytes, false);
    }
}

void StarletHardware::Write(uint32_t address, unsigned widthBytes, uint32_t value) {
    if (widthBytes != 4 || (address & 3u) != 0)
        throw UnsupportedAccess(address, widthBytes, true);
    switch (address) {
        case HwBoot0: boot0 = value; return;
        case HwSrnProt: sramProtection = value; return;
        default: throw UnsupportedAccess(address, widthBytes, true);
    }
}

} // namespace Broadway
