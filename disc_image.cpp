#include "disc_image.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#endif

namespace Broadway {
namespace {
constexpr uint64_t kPartitionTableOffset = 0x40000;
constexpr uint32_t kWiiMagic = 0x5D1C9EA3;
constexpr uint32_t kClusterSize = 0x8000;
constexpr uint32_t kClusterUserOffset = 0x400;
constexpr uint32_t kClusterUserSize = 0x7C00;

uint32_t BE32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void CheckRange(uint64_t offset, uint64_t size, uint64_t limit, const char* what) {
    if (offset > limit || size > limit - offset)
        throw std::runtime_error(std::string(what) + " extends beyond its containing image/partition.");
}

#ifdef _WIN32
void CheckNt(NTSTATUS status, const char* operation) {
    if (status < 0)
        throw std::runtime_error(std::string(operation) + " failed (NTSTATUS " +
                                 std::to_string(static_cast<uint32_t>(status)) + ").");
}
#endif
}

WiiDiscImage::WiiDiscImage(const std::filesystem::path& path)
    : file(path, std::ios::binary | std::ios::in) {
    if (!file) throw std::runtime_error("Could not open Wii disc image.");
    file.seekg(0, std::ios::end);
    const auto end = file.tellg();
    if (end < 0) throw std::runtime_error("Could not determine Wii disc image size.");
    imageSize = static_cast<uint64_t>(end);
    if (imageSize < kPartitionTableOffset + 0x20)
        throw std::runtime_error("File is too short to contain a Wii partition table.");

    std::array<uint8_t, 0x62> disc{};
    ReadAt(0, disc.data(), disc.size());
    header.gameId.assign(reinterpret_cast<const char*>(disc.data()), 6);
    header.discNumber = disc[6];
    header.revision = disc[7];
    header.magic = BE32(disc.data() + 0x18);
    header.encrypted = disc[0x61] == 0;
    if (header.magic != kWiiMagic)
        throw std::runtime_error("Wii disc magic 0x5D1C9EA3 was not present in the image header.");

    std::array<uint8_t, 0x20> tableHeader{};
    ReadAt(kPartitionTableOffset, tableHeader.data(), tableHeader.size());
    for (uint32_t group = 0; group < 4; ++group) {
        const uint32_t count = BE32(tableHeader.data() + group * 8);
        const uint32_t tableWordOffset = BE32(tableHeader.data() + 4 + group * 8);
        if (count == 0) continue;
        const uint64_t tableOffset = uint64_t(tableWordOffset) * 4;
        if (count > 4096) throw std::runtime_error("Partition table has an implausible entry count.");
        CheckRange(tableOffset, uint64_t(count) * 8, imageSize, "Partition entries");
        std::vector<uint8_t> entries(size_t(count) * 8);
        if (!entries.empty()) ReadAt(tableOffset, entries.data(), entries.size());
        for (uint32_t n = 0; n < count; ++n) {
            const uint8_t* entry = entries.data() + size_t(n) * 8;
            const uint64_t partitionOffset = uint64_t(BE32(entry)) * 4;
            const uint32_t type = BE32(entry + 4);
            CheckRange(partitionOffset, 0x2C0, imageSize, "Partition metadata");
            partitions.push_back({partitionOffset, type});
        }
    }
    if (partitions.empty()) throw std::runtime_error("Wii disc contains no partition entries.");
}

void WiiDiscImage::ReadAt(uint64_t offset, uint8_t* destination, size_t size) {
    CheckRange(offset, size, imageSize, "Disc read");
    if (offset > uint64_t(std::numeric_limits<std::streamoff>::max()))
        throw std::runtime_error("Disc offset cannot be represented by the host file API.");
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file || (size && !file.read(reinterpret_cast<char*>(destination), static_cast<std::streamsize>(size))))
        throw std::runtime_error("Short read from Wii disc image.");
}

std::vector<uint8_t> WiiDiscImage::ReadRaw(uint64_t offset, size_t size) {
    std::vector<uint8_t> result(size);
    if (size) ReadAt(offset, result.data(), size);
    return result;
}

void WiiDiscImage::AesCbcDecrypt(const std::array<uint8_t, 16>& key,
                                 std::array<uint8_t, 16> iv,
                                 uint8_t* data, size_t size) {
    if (size % 16) throw std::runtime_error("AES-CBC payload size must be a multiple of 16 bytes.");
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE keyHandle = nullptr;
    DWORD objectSize = 0, returned = 0;
    std::vector<uint8_t> keyObject;
    CheckNt(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0), "BCryptOpenAlgorithmProvider");
    try {
        CheckNt(BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
                                  reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                                  sizeof(BCRYPT_CHAIN_MODE_CBC), 0), "BCryptSetProperty(CBC)");
        CheckNt(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                  reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &returned, 0),
                "BCryptGetProperty(ObjectLength)");
        keyObject.resize(objectSize);
        CheckNt(BCryptGenerateSymmetricKey(algorithm, &keyHandle, keyObject.data(), objectSize,
                                            const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0),
                "BCryptGenerateSymmetricKey");
        ULONG done = 0;
        std::vector<uint8_t> ivCopy(iv.begin(), iv.end());
        CheckNt(BCryptDecrypt(keyHandle, data, static_cast<ULONG>(size), nullptr, ivCopy.data(),
                              static_cast<ULONG>(ivCopy.size()), data, static_cast<ULONG>(size), &done,
                              0), "BCryptDecrypt");
        if (done != size) throw std::runtime_error("AES-CBC returned a short plaintext block.");
        BCryptDestroyKey(keyHandle);
        BCryptCloseAlgorithmProvider(algorithm, 0);
    } catch (...) {
        if (keyHandle) BCryptDestroyKey(keyHandle);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
#else
    (void)key; (void)iv; (void)data; (void)size;
    throw std::runtime_error("Wii partition AES requires the Windows CNG backend in this build.");
#endif
}

std::array<uint8_t, 16> WiiDiscImage::DeriveTitleKey(
    size_t partitionIndex, const std::array<uint8_t, 16>& commonKey) {
    if (partitionIndex >= partitions.size()) throw std::out_of_range("Partition index is out of range.");
    std::array<uint8_t, 0x1F4> ticket{};
    ReadAt(partitions[partitionIndex].offset, ticket.data(), ticket.size());
    const uint8_t keyIndex = ticket[0x1F1];
    if (keyIndex != 0) throw std::runtime_error("Ticket selects a non-standard Wii common-key index unsupported by this build.");
    std::array<uint8_t, 16> titleKey{};
    std::copy_n(ticket.data() + 0x1BF, titleKey.size(), titleKey.data());
    std::array<uint8_t, 16> iv{};
    std::copy_n(ticket.data() + 0x1DC, 8, iv.data());
    AesCbcDecrypt(commonKey, iv, titleKey.data(), titleKey.size());
    return titleKey;
}

std::vector<uint8_t> WiiDiscImage::ReadPartitionData(
    size_t partitionIndex, uint64_t offset, size_t size,
    const std::array<uint8_t, 16>& titleKey) {
    if (partitionIndex >= partitions.size()) throw std::out_of_range("Partition index is out of range.");
    if (!size) return {};
    std::array<uint8_t, 0x2C0> metadata{};
    ReadAt(partitions[partitionIndex].offset, metadata.data(), metadata.size());
    const uint64_t dataOffset = uint64_t(BE32(metadata.data() + 0x2B8)) * 4;
    const uint64_t dataSize = uint64_t(BE32(metadata.data() + 0x2BC)) * 4;
    CheckRange(offset, size, dataSize, "Partition data request");
    std::vector<uint8_t> output(size);
    if (!header.encrypted) {
        ReadAt(partitions[partitionIndex].offset + dataOffset + offset, output.data(), size);
        return output;
    }
    size_t outputOffset = 0;
    while (outputOffset < size) {
        const uint64_t logicalOffset = offset + outputOffset;
        const uint64_t clusterIndex = logicalOffset / kClusterUserSize;
        const uint32_t withinCluster = uint32_t(logicalOffset % kClusterUserSize);
        const size_t amount = std::min<size_t>(size - outputOffset, kClusterUserSize - withinCluster);
        const uint64_t clusterFileOffset = partitions[partitionIndex].offset + dataOffset + clusterIndex * kClusterSize;
        CheckRange(clusterFileOffset, kClusterSize, imageSize, "Encrypted partition cluster");
        std::array<uint8_t, kClusterSize> cluster{};
        ReadAt(clusterFileOffset, cluster.data(), cluster.size());
        std::array<uint8_t, 0x400> hashBlock{};
        std::copy_n(cluster.data(), hashBlock.size(), hashBlock.data());
        AesCbcDecrypt(titleKey, {}, hashBlock.data(), hashBlock.size());
        std::vector<uint8_t> user(cluster.begin() + kClusterUserOffset,
                                  cluster.begin() + kClusterSize);
        std::array<uint8_t, 16> iv{};
        // The encrypted H2 tail is the chaining IV stored in the cluster header.
        std::copy_n(cluster.data() + 0x3D0, iv.size(), iv.data());
        AesCbcDecrypt(titleKey, iv, user.data(), user.size());
        std::copy_n(user.data() + withinCluster, amount, output.data() + outputOffset);
        outputOffset += amount;
    }
    return output;
}

WiiBootInformation WiiDiscImage::ReadBootInformation(
    size_t partitionIndex, const std::array<uint8_t, 16>& titleKey) {
    const auto boot = ReadPartitionData(partitionIndex, 0, 0x2444, titleKey);
    WiiBootInformation info;
    info.dolOffset = BE32(boot.data() + 0x420) * 4;
    info.fstOffset = BE32(boot.data() + 0x424) * 4;
    info.fstSize = BE32(boot.data() + 0x428) * 4;
    info.fstMaximumSize = BE32(boot.data() + 0x42C) * 4;
    info.apploaderOffset = 0x2440;
    const auto app = ReadPartitionData(partitionIndex, info.apploaderOffset, 0x20, titleKey);
    info.apploaderEntry = BE32(app.data() + 0x10);
    info.apploaderCodeSize = BE32(app.data() + 0x14);
    info.apploaderTrailerSize = BE32(app.data() + 0x18);
    if (info.fstSize && info.fstMaximumSize && info.fstSize > info.fstMaximumSize)
        throw std::runtime_error("Partition boot block reports an FST larger than its maximum size.");
    return info;
}

std::vector<WiiDiscFile> WiiDiscImage::ReadFileSystem(
    size_t partitionIndex, const WiiBootInformation& boot,
    const std::array<uint8_t, 16>& titleKey) {
    if (boot.fstSize < 12) throw std::runtime_error("Partition FST is smaller than its root entry.");
    const auto fst = ReadPartitionData(partitionIndex, boot.fstOffset, boot.fstSize, titleKey);
    const uint32_t rootWord = BE32(fst.data());
    if ((rootWord & 0xFF000000u) != 0x01000000u)
        throw std::runtime_error("Partition FST root entry is not a directory.");
    const uint32_t nodeCount = BE32(fst.data() + 8);
    if (nodeCount == 0 || uint64_t(nodeCount) * 12 > fst.size())
        throw std::runtime_error("Partition FST node count exceeds the loaded FST bounds.");
    const uint64_t nameTableOffset = uint64_t(nodeCount) * 12;
    std::vector<WiiDiscFile> files;
    files.reserve(nodeCount);
    files.push_back({"/", 0, 0, true});
    std::vector<std::pair<uint32_t, std::string>> directories{{nodeCount, "/"}};
    for (uint32_t index = 1; index < nodeCount; ++index) {
        while (directories.size() > 1 && index >= directories.back().first)
            directories.pop_back();
        const size_t entryOffset = size_t(index) * 12;
        const uint32_t typeAndName = BE32(fst.data() + entryOffset);
        const bool isDirectory = (typeAndName & 0x01000000u) != 0;
        const uint32_t nameOffset = typeAndName & 0x00FFFFFFu;
        if (nameTableOffset + nameOffset >= fst.size())
            throw std::runtime_error("FST filename offset is outside the name table.");
        const char* nameBegin = reinterpret_cast<const char*>(fst.data() + nameTableOffset + nameOffset);
        const size_t remaining = fst.size() - size_t(nameTableOffset + nameOffset);
        const void* terminator = std::memchr(nameBegin, 0, remaining);
        if (!terminator) throw std::runtime_error("FST filename is not terminated within the name table.");
        const auto* nameEnd = static_cast<const char*>(terminator);
        const std::string name(nameBegin, nameEnd);
        const std::string path = directories.back().second == "/"
            ? "/" + name : directories.back().second + "/" + name;
        if (isDirectory) {
            const uint32_t subtreeEnd = BE32(fst.data() + entryOffset + 8);
            if (subtreeEnd <= index || subtreeEnd > nodeCount)
                throw std::runtime_error("FST directory subtree boundary is invalid.");
            files.push_back({path, 0, 0, true});
            directories.push_back({subtreeEnd, path});
        } else {
            const uint32_t fileOffset = BE32(fst.data() + entryOffset + 4);
            const uint32_t fileSize = BE32(fst.data() + entryOffset + 8);
            files.push_back({path, fileOffset, fileSize, false});
        }
    }
    return files;
}

std::vector<uint8_t> WiiDiscImage::ReadApploaderImage(
    size_t partitionIndex, const WiiBootInformation& boot,
    const std::array<uint8_t, 16>& titleKey) {
    const uint64_t total = uint64_t(boot.apploaderCodeSize) + boot.apploaderTrailerSize;
    if (total > std::numeric_limits<size_t>::max())
        throw std::runtime_error("Apploader image is too large for host memory.");
    return ReadPartitionData(partitionIndex, uint64_t(boot.apploaderOffset) + 0x20,
                             static_cast<size_t>(total), titleKey);
}

} // namespace Broadway
