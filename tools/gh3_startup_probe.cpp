#include "../cpu.hpp"
#include "../executable_loader.hpp"
#include "../gx.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace {
const char* ExceptionName(Broadway::ExceptionKind kind) {
    switch (kind) {
    case Broadway::ExceptionKind::UnimplementedInstruction: return "unimplemented CPU instruction";
    case Broadway::ExceptionKind::InvalidInstruction: return "invalid CPU instruction";
    case Broadway::ExceptionKind::DataStorage: return "data-storage exception";
    case Broadway::ExceptionKind::InstructionStorage: return "instruction-storage exception";
    case Broadway::ExceptionKind::Alignment: return "alignment exception";
    case Broadway::ExceptionKind::Program: return "program exception";
    case Broadway::ExceptionKind::SystemCall: return "system call (no Wii OS service)";
    case Broadway::ExceptionKind::FloatingPointUnavailable: return "floating-point unavailable exception";
    }
    return "unknown CPU exception";
}
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: gh3_startup_probe <RGHP52 DATA/sys/main.dol> [instruction-limit]\n";
        return 2;
    }
    try {
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("Could not open the game DOL.");
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        Broadway::CPU cpu(32u * 1024u * 1024u);
        Broadway::VideoFrameMailbox frames;
        Broadway::GXProcessor gx(cpu.memory, frames);
        cpu.memory.AttachMmioDevice(&gx);
        const auto loaded = Broadway::ExecutableLoader::Load(cpu, bytes, Broadway::ExecutableFormat::Dol);
        std::cout << "Loaded " << loaded.formatName << " entry=0x" << std::hex << loaded.entryPoint
                  << " sections=" << std::dec << loaded.segmentCount
                  << " initial r1=0x" << std::hex << cpu.registers.GPR[1]
                  << " r2=0x" << cpu.registers.GPR[2]
                  << " r13=0x" << cpu.registers.GPR[13] << '\n';
        const uint64_t instructionLimit = argc == 3 ? std::stoull(argv[2]) : 1000000u;
        uint32_t lastSuccessfulPC = 0;
        uint32_t lastSuccessfulRaw = 0;
        for (uint64_t n = 0; n < instructionLimit; ++n) {
            const uint32_t executingPC = cpu.registers.PC;
            uint32_t executingRaw = 0;
            try { executingRaw = cpu.memory.Read32(executingPC); } catch (...) {}
            try {
                cpu.Step();
                lastSuccessfulPC = executingPC;
                lastSuccessfulRaw = executingRaw;
            } catch (const Broadway::CPUException& exception) {
                const uint32_t pc = exception.FaultPC();
                uint32_t raw = 0;
                try { raw = cpu.memory.Read32(pc); } catch (...) {}
                const auto decoded = Broadway::Decoder::Decode(raw);
                std::cout << "Stopped after " << std::dec << cpu.InstructionsExecuted()
                          << " instructions: " << ExceptionName(exception.Kind())
                          << " at PC=0x" << std::hex << pc << " raw=0x" << raw
                          << " mnemonic=" << Broadway::Decoder::Name(decoded) << '\n'
                          << "CPU detail: " << exception.what() << '\n'
                          << "Last successful: PC=0x" << std::hex << lastSuccessfulPC
                          << " raw=0x" << lastSuccessfulRaw << " "
                          << Broadway::Decoder::Format(Broadway::Decoder::Decode(lastSuccessfulRaw),lastSuccessfulPC) << '\n';
                if (!gx.LastError().empty()) std::cout << "GX detail: " << gx.LastError() << '\n';
                return 1;
            }
        }
        std::cout << "Reached instruction limit with PC=0x" << std::hex << cpu.registers.PC
                  << " last=0x" << cpu.LastInstructionPC() << " GX frames=" << std::dec
                  << gx.FramesCopied() << " GPR0=0x" << std::hex << cpu.registers.GPR[0]
                  << " GPR3=0x" << cpu.registers.GPR[3] << " CR=0x" << cpu.registers.CR
                  << " (DOL-only startup; no IOS/DI/VI/Starlet model)\n";
        const uint32_t contextStart = cpu.registers.PC >= 0x80 ? cpu.registers.PC - 0x80 : 0;
        for (uint32_t address = contextStart; address <= cpu.registers.PC + 8; address += 4) {
            try {
                const uint32_t raw=cpu.memory.Read32(address);
                const auto decoded=Broadway::Decoder::Decode(raw);
                std::cout << "  0x" << std::hex << address << "  0x" << raw << "  "
                          << Broadway::Decoder::Format(decoded,address) << '\n';
            } catch (...) { break; }
        }
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Startup probe failed: " << exception.what() << '\n';
        return 1;
    }
}
