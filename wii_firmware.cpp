#include "wii_firmware.hpp"

#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace Broadway {
namespace {
uint16_t Be16(const std::vector<uint8_t>& b, size_t p) {
    return uint16_t((uint16_t(b.at(p)) << 8) | b.at(p + 1));
}
uint32_t Be32(const std::vector<uint8_t>& b, size_t p) {
    return (uint32_t(b.at(p)) << 24) | (uint32_t(b.at(p + 1)) << 16) |
        (uint32_t(b.at(p + 2)) << 8) | b.at(p + 3);
}
uint64_t Be64(const std::vector<uint8_t>& b, size_t p) {
    return (uint64_t(Be32(b, p)) << 32) | Be32(b, p + 4);
}
std::string HexId(uint32_t id) {
    std::ostringstream out;
    out << std::hex << std::nouppercase << std::setw(8) << std::setfill('0') << id;
    return out.str();
}
std::optional<std::string> SharedContentId(const std::filesystem::path& root,
                                           const std::array<uint8_t, 20>& hash) {
    std::ifstream input(root / "shared1" / "content.map", std::ios::binary);
    if (!input) return std::nullopt;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    constexpr size_t kRecordSize = 28; // eight ASCII hex digits and the TMD SHA-1
    if (bytes.size() % kRecordSize != 0) return std::nullopt;
    for (size_t p = 0; p < bytes.size(); p += kRecordSize) {
        std::string id(reinterpret_cast<const char*>(bytes.data() + p), 8);
        for (char c : id)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return std::nullopt;
        bool matches = true;
        for (size_t i = 0; i < hash.size(); ++i)
            if (bytes[p + 8 + i] != hash[i]) matches = false;
        if (matches) return id;
    }
    return std::nullopt;
}
std::optional<std::filesystem::path> ContentPath(const std::filesystem::path& root,
                                                 const std::filesystem::path& titleDir,
                                                 const WiiTmdContent& content) {
    if ((content.type & 0x8000u) != 0) {
        const auto sharedId = SharedContentId(root, content.sha1);
        if (!sharedId) return std::nullopt;
        return root / "shared1" / (*sharedId + ".app");
    }
    return titleDir / "content" / (HexId(content.id) + ".app");
}
const WiiTmdContent& BootContent(const WiiTitleMetadata& tmd) {
    for (const auto& content : tmd.contents)
        if (content.index == tmd.bootIndex)
            return content;
    throw std::runtime_error("TMD boot index does not name a content record.");
}
WiiTitleMetadata ReadTitle(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not read title metadata: " + path.string());
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    return WiiFirmwareInspector::ParseTmd(bytes);
}
}

WiiTitleMetadata WiiFirmwareInspector::ParseTmd(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 4) throw std::runtime_error("TMD is truncated before its signature type.");
    size_t body = 0;
    switch (Be32(bytes, 0)) {
    case 0x00010000: body = 0x240; break; // RSA-4096 signature block
    case 0x00010001: body = 0x140; break; // RSA-2048 signature block
    case 0x00010002: body = 0x080; break; // ECC signature block
    default: throw std::runtime_error("Unsupported TMD signature type.");
    }
    constexpr size_t kTmdFixedBody = 0xA4;
    constexpr size_t kContentRecordSize = 0x24;
    if (bytes.size() < body + kTmdFixedBody)
        throw std::runtime_error("TMD fixed header is truncated.");

    WiiTitleMetadata tmd;
    tmd.systemVersion = Be64(bytes, body + 0x44);
    tmd.titleId = Be64(bytes, body + 0x4c);
    tmd.titleVersion = Be16(bytes, body + 0x9c);
    const uint16_t count = Be16(bytes, body + 0x9e);
    tmd.bootIndex = Be16(bytes, body + 0xa0);
    const size_t records = body + kTmdFixedBody;
    if (size_t(count) > (std::numeric_limits<size_t>::max() - records) / kContentRecordSize ||
        bytes.size() < records + size_t(count) * kContentRecordSize)
        throw std::runtime_error("TMD content table is truncated or invalid.");
    tmd.contents.reserve(count);
    bool bootFound = false;
    for (uint16_t i = 0; i < count; ++i) {
        const size_t p = records + size_t(i) * kContentRecordSize;
        WiiTmdContent c;
        c.id = Be32(bytes, p);
        c.index = Be16(bytes, p + 4);
        c.type = Be16(bytes, p + 6);
        c.size = Be64(bytes, p + 8);
        for (size_t hash = 0; hash < c.sha1.size(); ++hash) c.sha1[hash] = bytes[p + 16 + hash];
        for (const auto& prior : tmd.contents)
            if (prior.index == c.index) throw std::runtime_error("TMD contains duplicate content indices.");
        if (c.index == tmd.bootIndex) bootFound = true;
        tmd.contents.push_back(c);
    }
    if (!bootFound) throw std::runtime_error("TMD boot index does not name a content record.");
    return tmd;
}

WiiFirmwareInventory WiiFirmwareInspector::InspectNand(const std::filesystem::path& root) {
    WiiFirmwareInventory result;
    result.nandRoot = root;
    const auto menuDir = root / "title" / "00000001" / "00000002";
    result.systemMenuTmdPath = menuDir / "content" / "title.tmd";
    result.systemMenuTicketPath = root / "ticket" / "00000001" / "00000002.tik";
    result.systemMenuTmdPresent = std::filesystem::is_regular_file(result.systemMenuTmdPath);
    result.systemMenuTicketPresent = std::filesystem::is_regular_file(result.systemMenuTicketPath);
    if (!result.systemMenuTmdPresent) {
        result.status = "System Menu TMD not found in the selected extracted NAND root.";
        return result;
    }

    const WiiTitleMetadata menu = ReadTitle(result.systemMenuTmdPath);
    if (menu.titleId != 0x0000000100000002ull)
        throw std::runtime_error("The selected System Menu TMD has an unexpected title ID.");
    if (const auto menuBoot = ContentPath(root, menuDir, BootContent(menu))) {
        result.systemMenuBootContentPath = *menuBoot;
        result.systemMenuBootContentPresent = std::filesystem::is_regular_file(*menuBoot);
    }
    result.requiredIosTitleId = menu.systemVersion;
    if ((result.requiredIosTitleId >> 32) != 1 || uint32_t(result.requiredIosTitleId) == 0)
        throw std::runtime_error("System Menu TMD does not identify an IOS title in System Version.");

    const uint32_t iosSlot = uint32_t(result.requiredIosTitleId);
    const auto iosDir = root / "title" / "00000001" / HexId(iosSlot);
    result.iosTmdPath = iosDir / "content" / "title.tmd";
    result.iosTicketPath = root / "ticket" / "00000001" / (HexId(iosSlot) + ".tik");
    result.iosTmdPresent = std::filesystem::is_regular_file(result.iosTmdPath);
    result.iosTicketPresent = std::filesystem::is_regular_file(result.iosTicketPath);
    if (result.iosTmdPresent) {
        const WiiTitleMetadata ios = ReadTitle(result.iosTmdPath);
        if (ios.titleId != result.requiredIosTitleId)
            throw std::runtime_error("IOS TMD title ID does not match System Menu's required IOS.");
        result.iosContentFilesTotal = ios.contents.size();
        for (const auto& content : ios.contents) {
            const auto path = ContentPath(root, iosDir, content);
            if (path && std::filesystem::is_regular_file(*path)) ++result.iosContentFilesPresent;
            else result.missingIosContentPaths.push_back(path.value_or(root / "shared1" / "content.map"));
        }
        if (const auto iosBoot = ContentPath(root, iosDir, BootContent(ios))) {
            result.iosBootContentPath = *iosBoot;
            result.iosBootContentPresent = std::filesystem::is_regular_file(*iosBoot);
        }
    }
    result.requiredFilesPresent = result.systemMenuBootContentPresent && result.systemMenuTicketPresent &&
        result.iosTmdPresent && result.iosContentFilesPresent == result.iosContentFilesTotal &&
        result.iosContentFilesTotal != 0 && result.iosTicketPresent;
    std::ostringstream status;
    status << "System Menu metadata " << (result.systemMenuTmdPresent ? "found" : "missing")
        << "; IOS slot " << iosSlot << " metadata " << (result.iosTmdPresent ? "found" : "missing")
        << "; IOS content files " << result.iosContentFilesPresent << '/' << result.iosContentFilesTotal
        << "; boot2 is not represented by this extracted-title inventory; boot runtime is not implemented";
    result.status = status.str();
    return result;
}

WiiSystemEnvironment WiiFirmwareInspector::InspectEnvironment(const std::filesystem::path& root) {
    WiiSystemEnvironment env;
    env.nandRoot = root;
    const auto inventory = InspectNand(root);
    env.systemMenuTitleId = 0x0000000100000002ull;
    env.requiredIosTitleId = inventory.requiredIosTitleId;
    env.hasSystemMenuTmd = inventory.systemMenuTmdPresent;
    env.hasSystemMenuBootContent = inventory.systemMenuBootContentPresent;
    env.hasSystemMenuTicket = inventory.systemMenuTicketPresent;
    env.hasIosTmd = inventory.iosTmdPresent;
    env.hasIosBootContent = inventory.iosBootContentPresent;
    env.hasIosTicket = inventory.iosTicketPresent;
    env.hasAllRequiredFiles = inventory.requiredFilesPresent;
    env.iosContentFilesPresent = inventory.iosContentFilesPresent;
    env.iosContentFilesTotal = inventory.iosContentFilesTotal;

    // Genuine Wii firmware data can exist in the extracted NAND tree without a full
    // boot2/IOS runtime being implemented. This classification reflects the real title
    // inventory that is available to the emulator and the remaining boot runtime gap.
    if (std::filesystem::exists(root / "boot2") || std::filesystem::exists(root / "boot2" / "boot2.bin"))
        env.hasBoot2 = true;
    if (!env.hasSystemMenuTmd) {
        env.bootStage = WiiBootStage::SystemMenuMissing;
        env.status = "System Menu metadata is not present in the extracted NAND tree.";
        return env;
    }
    if (!env.hasIosTmd) {
        env.bootStage = WiiBootStage::IosMetadataMissing;
        env.status = "The required IOS title is not present in the extracted NAND tree.";
        return env;
    }
    if (!env.hasBoot2) {
        env.bootStage = WiiBootStage::Boot2Missing;
        env.status = "Extracted NAND contains the System Menu and IOS files, but boot2 is not present or not represented in this inventory.";
        return env;
    }
    if (env.hasAllRequiredFiles) {
        env.bootStage = WiiBootStage::BootFilesAvailable;
        env.status = "Boot2 and the required Wii title metadata are available; actual boot runtime remains pending implementation.";
        return env;
    }
    env.bootStage = WiiBootStage::RuntimePending;
    env.status = inventory.status;
    return env;
}

WiiSystemEnvironment WiiSystemEnvironment::Inspect(const std::filesystem::path& root) {
    return WiiFirmwareInspector::InspectEnvironment(root);
}

} // namespace Broadway
