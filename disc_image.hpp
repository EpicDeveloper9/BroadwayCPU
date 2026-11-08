#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Broadway {

struct DiscPartition {
    uint64_t offset = 0;
    uint32_t type = 0;
};

struct WiiDiscHeader {
    std::string gameId;
    uint8_t discNumber = 0;
    uint8_t revision = 0;
    uint32_t magic = 0;
    bool encrypted = true;
};

struct WiiBootInformation {
    uint32_t dolOffset = 0;
    uint32_t fstOffset = 0;
    uint32_t fstSize = 0;
    uint32_t fstMaximumSize = 0;
    uint32_t apploaderOffset = 0;
    uint32_t apploaderEntry = 0;
    uint32_t apploaderCodeSize = 0;
    uint32_t apploaderTrailerSize = 0;
};

struct WiiDiscFile {
    std::string path;
    uint32_t offset = 0;
    uint32_t size = 0;
    bool directory = false;
};

// Reads raw 1:1 Wii ISO images. Encrypted partition data needs the title key;
// the common key is supplied by the caller so no console key is embedded here.
class WiiDiscImage {
public:
    explicit WiiDiscImage(const std::filesystem::path& path);

    const WiiDiscHeader& Header() const noexcept { return header; }
    const std::vector<DiscPartition>& Partitions() const noexcept { return partitions; }
    uint64_t ImageSize() const noexcept { return imageSize; }

    std::vector<uint8_t> ReadRaw(uint64_t offset, size_t size);
    std::array<uint8_t, 16> DeriveTitleKey(size_t partitionIndex,
                                           const std::array<uint8_t, 16>& commonKey);
    std::vector<uint8_t> ReadPartitionData(size_t partitionIndex, uint64_t offset,
                                           size_t size,
                                           const std::array<uint8_t, 16>& titleKey);
    WiiBootInformation ReadBootInformation(size_t partitionIndex,
                                           const std::array<uint8_t, 16>& titleKey);
    std::vector<WiiDiscFile> ReadFileSystem(size_t partitionIndex,
                                            const WiiBootInformation& boot,
                                            const std::array<uint8_t, 16>& titleKey);
    std::vector<uint8_t> ReadApploaderImage(size_t partitionIndex,
                                            const WiiBootInformation& boot,
                                            const std::array<uint8_t, 16>& titleKey);

private:
    std::ifstream file;
    uint64_t imageSize = 0;
    WiiDiscHeader header;
    std::vector<DiscPartition> partitions;

    void ReadAt(uint64_t offset, uint8_t* destination, size_t size);
    static void AesCbcDecrypt(const std::array<uint8_t, 16>& key,
                              std::array<uint8_t, 16> iv,
                              uint8_t* data, size_t size);
};

} // namespace Broadway
