#include "../arm_elf.hpp"
#include "../starlet_hardware.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: starlet_trace <ios-content|elf|raw> [raw-address] [limit]\n";
        return 2;
    }
    std::string path = argv[1];
    uint32_t rawAddress = (argc > 2) ? static_cast<uint32_t>(std::stoul(argv[2], nullptr, 0)) : 0;
    uint64_t limit = (argc > 3) ? std::stoull(argv[3]) : 1000;
    struct TraceEntry {
        uint64_t count;
        uint32_t pc;
        uint32_t instruction;
        uint32_t cpsr;
        uint32_t r0;
        uint32_t r1;
        bool thumb;
    };
    std::vector<TraceEntry> recent;
    auto data = read_file(path);
    Broadway::StarletArm arm(4ull * 1024u * 1024u * 1024u);
    std::vector<Broadway::ArmElfSegment> loadedSegments;
    Broadway::StarletHardware hardware;
    arm.mmioReadAccess = [&](uint32_t address, unsigned width) {
        return hardware.Read(address, width);
    };
    arm.mmioWriteAccess = [&](uint32_t address, unsigned width, uint32_t value) {
        hardware.Write(address, width, value);
    };
    bool isElf = data.size() >= 4 && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F';
    const bool isIosContent = data.size() >= 16 && data[0] == 0 && data[1] == 0 &&
                              data[2] == 0 && data[3] == 16;
    if (isIosContent || isElf) {
        auto image = isIosContent
            ? Broadway::ArmElfLoader::ParseIosBootContent(data)
            : Broadway::ArmElfLoader::Parse(data);
        loadedSegments = image.segments;
        arm.Load(image);
        std::cout << (isIosContent ? "IOS ELF entry=0x" : "ELF entry=0x")
                  << std::hex << image.entry << " segments=" << std::dec << image.segments.size() << "\n";
        if (isIosContent)
            std::cout << "pre-ELF IOS payload bytes=0x" << std::hex << image.bootPayload.size()
                      << " (retained; no verified load address available)\n";
        for (auto& s : image.segments) {
            std::cout << "  seg va=0x" << std::hex << s.address << " pa=0x" << s.physicalAddress
                      << " filesz=0x" << s.fileSize << " memsz=0x" << s.memorySize << "\n";
        }
    } else {
        arm.LoadBinary(data, rawAddress);
        arm.R[15] = rawAddress;
        std::cout << "raw loaded at 0x" << std::hex << rawAddress << " size=0x" << data.size() << "\n";
    }

    uint64_t count = arm.InstructionCount();
    try {
        while (count < limit && !arm.halted) {
            uint32_t pc = arm.R[15];
            if (!loadedSegments.empty() && std::none_of(
                    loadedSegments.begin(), loadedSegments.end(),
                    [pc](const Broadway::ArmElfSegment& segment) {
                        const uint32_t loadAddress = segment.physicalAddress
                            ? segment.physicalAddress : segment.address;
                        return (pc >= segment.address &&
                                uint64_t(pc) < uint64_t(segment.address) + segment.memorySize) ||
                               (pc >= loadAddress &&
                                uint64_t(pc) < uint64_t(loadAddress) + segment.memorySize);
                    })) {
                std::cout << "STOP: PC 0x" << std::hex << pc
                          << " is outside loaded ELF segments; IOS pre-ELF payload/boot2 setup is not applied\n";
                break;
            }
            bool thumb = arm.Thumb();
            uint32_t ins = thumb ? arm.Read16(pc) : arm.Read32(pc);
            recent.push_back({count, pc, ins, arm.CPSR, arm.R[0], arm.R[1], thumb});
            if (recent.size() > 16) recent.erase(recent.begin());
            if (count < 64 || (count % 10000) == 0)
                std::cout << std::dec << count << " pc=0x" << std::hex << pc << " "
                          << (thumb ? "T" : "A") << " ins=0x" << ins << "\n";
            arm.Step();
            count = arm.InstructionCount();
        }
    } catch (const std::exception& e) {
        std::cout << "EXCEPTION after " << std::dec << count << ": " << e.what() << "\n";
        std::cout << "PC=0x" << std::hex << arm.R[15] << " CPSR=0x" << arm.CPSR << "\n";
    }
    std::cout << "recent instructions:\n";
    for (const auto& entry : recent)
        std::cout << std::dec << entry.count << " pc=0x" << std::hex << entry.pc << " "
                  << (entry.thumb ? "T" : "A") << " ins=0x" << entry.instruction
                  << " cpsr=0x" << entry.cpsr << " r0=0x" << entry.r0
                  << " r1=0x" << entry.r1 << "\n";
    std::cout << "done count=" << std::dec << count << " halted=" << arm.halted
              << " PC=0x" << std::hex << arm.R[15] << " CPSR=0x" << arm.CPSR
              << " mode=0x" << arm.Mode()
              << " r0=0x" << arm.R[0] << " r1=0x" << arm.R[1]
              << " r2=0x" << arm.R[2] << " r3=0x" << arm.R[3]
              << " sp=0x" << arm.R[13] << " lr=0x" << arm.R[14] << "\n";
    return 0;
}