#include "../arm_elf.hpp"

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
        std::cerr << "Usage: starlet_trace <elf-or-raw> [raw-address] [limit]\n";
        return 2;
    }
    std::string path = argv[1];
    uint32_t rawAddress = (argc > 2) ? static_cast<uint32_t>(std::stoul(argv[2], nullptr, 0)) : 0;
    uint64_t limit = (argc > 3) ? std::stoull(argv[3]) : 1000;
    auto data = read_file(path);
    Broadway::StarletArm arm(4ull * 1024u * 1024u * 1024u);
    bool isElf = data.size() >= 4 && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F';
    if (isElf) {
        auto image = Broadway::ArmElfLoader::Parse(data);
        arm.Load(image);
        std::cout << "ELF entry=0x" << std::hex << image.entry << " segments=" << std::dec << image.segments.size() << "\n";
        for (auto& s : image.segments) {
            std::cout << "  seg va=0x" << std::hex << s.address << " filesz=0x" << s.fileSize << " memsz=0x" << s.memorySize << "\n";
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
            bool thumb = arm.Thumb();
            uint32_t ins = thumb ? arm.Read16(pc) : arm.Read32(pc);
            std::cout << std::dec << count << " pc=0x" << std::hex << pc << " " << (thumb ? "T" : "A") << " ins=0x" << ins << "\n";
            arm.Step();
            count = arm.InstructionCount();
        }
    } catch (const std::exception& e) {
        std::cout << "EXCEPTION after " << std::dec << count << ": " << e.what() << "\n";
        std::cout << "PC=0x" << std::hex << arm.R[15] << " CPSR=0x" << arm.CPSR << "\n";
    }
    std::cout << "done count=" << std::dec << count << " halted=" << arm.halted << "\n";
    return 0;
}