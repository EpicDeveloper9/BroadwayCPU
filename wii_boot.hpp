#pragma once

#include "disc_image.hpp"
#include "memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace Broadway {

class CPU;
class GXProcessor;

struct WiiBootResult {
    std::string gameId;
    uint8_t revision = 0;
    uint32_t apploaderEntry = 0;
    uint32_t gameEntry = 0;
    uint64_t instructionsExecuted = 0;
    uint64_t discReadRequests = 0;
    uint64_t discBytesLoaded = 0;
};

// Initializes only boot fields with documented values or values read from
// the selected image. Revolution OS-owned structures and vectors are left to
// the system software that normally installs them.
void InitializeWiiBootMemory(Memory& memory,
                             const std::array<uint8_t, 0x20>& discHeader,
                             uint64_t partitionOffsetBytes,
                             uint32_t partitionType);

// Implements the IPL-to-apploader handoff: the apploader executes on the CPU,
// while its returned read requests are serviced by the decrypted disc reader.
class WiiBootSession {
public:
    WiiBootSession(const std::filesystem::path& imagePath,
                   const std::array<uint8_t, 16>& commonKey);

    const WiiDiscHeader& Header() const noexcept { return disc.Header(); }
    const WiiBootInformation& BootInformation() const noexcept { return boot; }
    size_t GamePartition() const noexcept { return gamePartition; }
    WiiBootResult Boot(CPU& cpu, GXProcessor& gx);

private:
    WiiDiscImage disc;
    size_t gamePartition = 0;
    std::array<uint8_t, 16> titleKey{};
    WiiBootInformation boot{};

    void Call(CPU& cpu, uint32_t function, uint32_t r3, uint32_t r4,
              uint32_t r5, uint64_t instructionLimit);
};

} // namespace Broadway
