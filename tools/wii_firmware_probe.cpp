#include "../wii_firmware.hpp"

#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: wii_firmware_probe <extracted-nand-root>\n";
        return 2;
    }
    try {
        const auto inventory = Broadway::WiiFirmwareInspector::InspectNand(argv[1]);
        std::cout << "NAND root: " << inventory.nandRoot.string() << '\n'
                  << "System Menu TMD: " << (inventory.systemMenuTmdPresent ? "present" : "missing") << '\n'
                  << "System Menu boot content: " << (inventory.systemMenuBootContentPresent ? "present" : "missing") << '\n'
                  << "Required IOS title: 0x" << std::hex << inventory.requiredIosTitleId << std::dec << '\n'
                  << "IOS TMD: " << (inventory.iosTmdPresent ? "present" : "missing") << '\n'
                  << "IOS content files: " << inventory.iosContentFilesPresent << '/' << inventory.iosContentFilesTotal << '\n'
                  << "IOS boot content: " << (inventory.iosBootContentPresent ? "present" : "missing") << '\n'
                  << "System Menu ticket: " << (inventory.systemMenuTicketPresent ? "present" : "missing") << '\n'
                  << "IOS ticket: " << (inventory.iosTicketPresent ? "present" : "missing") << '\n'
                  << "Required extracted files present: " << (inventory.requiredFilesPresent ? "yes" : "no") << '\n'
                  << "Status: " << inventory.status << '\n';
        for (const auto& path : inventory.missingIosContentPaths)
            std::cout << "Missing IOS content: " << path.string() << '\n';
        return inventory.requiredFilesPresent ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Firmware inspection failed: " << error.what() << '\n';
        return 3;
    }
}
