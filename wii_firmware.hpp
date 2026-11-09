#pragma once

#include <cstdint>
#include <filesystem>
#include <array>
#include <string>
#include <vector>

namespace Broadway {

struct WiiTmdContent {
    uint32_t id = 0;
    uint16_t index = 0;
    uint16_t type = 0;
    uint64_t size = 0;
    std::array<uint8_t, 20> sha1{};
};

struct WiiTitleMetadata {
    uint64_t systemVersion = 0;
    uint64_t titleId = 0;
    uint16_t titleVersion = 0;
    uint16_t bootIndex = 0;
    std::vector<WiiTmdContent> contents;
};

// Structural inventory only: this deliberately does not authenticate TMDs,
// decrypt title contents, or execute proprietary system software.
struct WiiFirmwareInventory {
    std::filesystem::path nandRoot;
    std::filesystem::path systemMenuTmdPath;
    std::filesystem::path systemMenuBootContentPath;
    std::filesystem::path systemMenuTicketPath;
    std::filesystem::path iosTmdPath;
    std::filesystem::path iosBootContentPath;
    std::filesystem::path iosTicketPath;
    uint64_t requiredIosTitleId = 0;
    bool systemMenuTmdPresent = false;
    bool systemMenuBootContentPresent = false;
    bool systemMenuTicketPresent = false;
    bool iosTmdPresent = false;
    bool iosBootContentPresent = false;
    bool iosTicketPresent = false;
    size_t iosContentFilesPresent = 0;
    size_t iosContentFilesTotal = 0;
    std::vector<std::filesystem::path> missingIosContentPaths;
    bool requiredFilesPresent = false;
    std::string status;
};

enum class WiiBootStage {
    Boot2Missing,
    SystemMenuMissing,
    IosMetadataMissing,
    BootFilesAvailable,
    RuntimePending
};

struct WiiSystemEnvironment {
    std::filesystem::path nandRoot;
    uint64_t systemMenuTitleId = 0;
    uint64_t requiredIosTitleId = 0;
    bool hasBoot2 = false;
    bool hasSystemMenuTmd = false;
    bool hasSystemMenuBootContent = false;
    bool hasSystemMenuTicket = false;
    bool hasIosTmd = false;
    bool hasIosBootContent = false;
    bool hasIosTicket = false;
    bool hasAllRequiredFiles = false;
    size_t iosContentFilesPresent = 0;
    size_t iosContentFilesTotal = 0;
    WiiBootStage bootStage = WiiBootStage::RuntimePending;
    std::string status;

    static WiiSystemEnvironment Inspect(const std::filesystem::path& root);
};

class WiiFirmwareInspector {
public:
    static WiiTitleMetadata ParseTmd(const std::vector<uint8_t>& bytes);
    static WiiFirmwareInventory InspectNand(const std::filesystem::path& root);
    static WiiSystemEnvironment InspectEnvironment(const std::filesystem::path& root);
};

} // namespace Broadway
