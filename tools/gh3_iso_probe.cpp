#include "../disc_image.hpp"
#include "../cpu.hpp"
#include "../decoder.hpp"
#include "../executable_loader.hpp"
#include "../gx.hpp"
#include "../wii_boot.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <map>
#include <vector>
#include <string>

namespace {
bool ParseKey(const std::string& text, std::array<uint8_t, 16>& key) {
    if (text.size() != 32) return false;
    for (size_t i = 0; i < key.size(); ++i) {
        try { key[i] = static_cast<uint8_t>(std::stoul(text.substr(i * 2, 2), nullptr, 16)); }
        catch (...) { return false; }
    }
    return true;
}
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 5) {
        std::cerr << "Usage: gh3_iso_probe <RGHP52.iso> [common-key-hex] [--direct-dol-probe [instruction-limit]]\n";
        return 2;
    }
    try {
        Broadway::WiiDiscImage disc(argv[1]);
        const auto& header = disc.Header();
        std::cout << "Disc ID: " << header.gameId << " disc=" << unsigned(header.discNumber)
                  << " revision=" << unsigned(header.revision) << " magic=0x" << std::hex << header.magic
                  << " size=0x" << disc.ImageSize() << std::dec << '\n';
        size_t gamePartition = disc.Partitions().size();
        for (size_t i = 0; i < disc.Partitions().size(); ++i) {
            const auto& partition = disc.Partitions()[i];
            std::cout << "Partition " << i << ": offset=0x" << std::hex << partition.offset
                      << " type=" << std::dec << partition.type << '\n';
            if (partition.type == 0 && gamePartition == disc.Partitions().size()) gamePartition = i;
        }
        if (header.gameId != "RGHP52" || header.revision != 1)
            throw std::runtime_error("The selected image is not Guitar Hero III Europe revision 1 (RGHP52). ");
        if (gamePartition == disc.Partitions().size())
            throw std::runtime_error("No data/game partition was found in the disc partition table.");
        std::cout << "Selected game partition: " << gamePartition << '\n';
        std::array<uint8_t, 16> commonKey{};
        const std::string mode = argc >= 4 ? argv[3] : "";
        const bool probeDol = mode == "--direct-dol-probe";
        const bool probeApploader = mode == "--apploader-probe";
        if (header.encrypted && argc < 3) {
            std::cerr << "Boot stopped before CPU reset: this retail ISO is encrypted and no common key was supplied.\n";
            return 3;
        }
        if (header.encrypted && !ParseKey(argv[2], commonKey))
            throw std::runtime_error("Common key must be exactly 32 hexadecimal digits.");
        std::array<uint8_t, 16> titleKey{};
        if (header.encrypted) titleKey = disc.DeriveTitleKey(gamePartition, commonKey);
        const auto boot = disc.ReadBootInformation(gamePartition, titleKey);
        std::cout << "Boot data: DOL offset=0x" << std::hex << boot.dolOffset
                  << " FST offset=0x" << boot.fstOffset << " FST size=0x" << boot.fstSize
                  << " FST max=0x" << boot.fstMaximumSize << " apploader offset=0x"
                  << boot.apploaderOffset << " entry=0x" << boot.apploaderEntry
                  << " code size=0x" << boot.apploaderCodeSize << " trailer=0x"
                  << boot.apploaderTrailerSize << std::dec << '\n';
        const auto fstEntries = disc.ReadFileSystem(gamePartition, boot, titleKey);
        const auto apploader = disc.ReadApploaderImage(gamePartition, boot, titleKey);
        size_t relFiles = 0, otherExecutableFiles = 0;
        for (const auto& entry : fstEntries) {
            if (entry.directory) continue;
            std::string lowerPath = entry.path;
            std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            if (lowerPath.ends_with(".rel")) ++relFiles;
            if (lowerPath.ends_with(".dol") || lowerPath.ends_with(".elf") ||
                lowerPath.ends_with(".rso")) ++otherExecutableFiles;
        }
        std::cout << "FST entries=" << fstEntries.size() << " apploader image bytes=" << apploader.size()
                  << " REL files=" << relFiles << " additional DOL/ELF/RSO files="
                  << otherExecutableFiles << '\n';
        if (!probeDol && !probeApploader) {
            std::cout << "CPU execution not started: this mode validates disc access and boot metadata. "
                         "Use --apploader-probe to execute the apploader, or --direct-dol-probe for an isolated DOL test.\n";
            return 4;
        }

        Broadway::CPU cpu(32u * 1024u * 1024u);
        Broadway::VideoFrameMailbox frames;
        Broadway::GXProcessor gx(cpu.memory, frames);
        cpu.memory.AttachMmioDevice(&gx);
        if (probeApploader) {
            try {
                Broadway::WiiBootSession session(argv[1], commonKey);
                const auto result = session.Boot(cpu, gx);
                std::cout << "Apploader executed: " << result.instructionsExecuted << " CPU instructions, "
                          << result.discReadRequests << " DVD read callbacks, " << result.discBytesLoaded
                          << " bytes loaded; game DOL entry=0x" << std::hex << result.gameEntry << std::dec << "\n";
            } catch (const Broadway::CPUException& e) {
                uint32_t raw = 0;
                try { raw = cpu.memory.Read32(e.FaultPC()); } catch (...) {}
                std::cout << "Apploader stopped after " << std::dec << cpu.InstructionsExecuted()
                          << " instructions: PC=0x" << std::hex << e.FaultPC() << " raw=0x" << raw
                          << " " << Broadway::Decoder::Format(Broadway::Decoder::Decode(raw), e.FaultPC())
                          << " : " << e.what() << std::dec << "\n"
                          << "LR=0x" << std::hex << cpu.registers.LR << " r0=0x" << cpu.registers.GPR[0]
                          << " r1=0x" << cpu.registers.GPR[1]
                          << " r2=0x" << cpu.registers.GPR[2] << " r3=0x" << cpu.registers.GPR[3]
                          << " r4=0x" << cpu.registers.GPR[4] << " r5=0x" << cpu.registers.GPR[5]
                          << " r6=0x" << cpu.registers.GPR[6] << " r7=0x" << cpu.registers.GPR[7] << "\n";
                std::cout << "CPU state after fault: PC=0x" << std::hex << cpu.registers.PC
                          << " SRR0=0x" << cpu.registers.SRR0 << " SRR1=0x" << cpu.registers.SRR1
                          << " MSR=0x" << cpu.registers.MSR << std::dec
                          << " | GX commands=" << gx.CommandsProcessed()
                          << " EFB-to-XFB copies=" << gx.FramesCopied()
                          << " frames submitted=" << gx.FramesCopied()
                          << " | apploader callbacks/DOL load not completed\n";
                const uint32_t start = e.FaultPC() >= 0x20 ? e.FaultPC() - 0x20 : 0;
                for (uint32_t address = start; address <= e.FaultPC() + 0x20; address += 4) {
                    const uint32_t word = cpu.memory.Read32(address);
                    std::cout << "0x" << std::hex << address << " 0x" << word << " "
                              << Broadway::Decoder::Format(Broadway::Decoder::Decode(word), address) << "\n";
                }
                std::cout << "Call site near LR=0x" << std::hex << cpu.registers.LR << ":\n";
                const uint32_t callerStart = (cpu.registers.LR >= 0x20 ? cpu.registers.LR - 0x20 : cpu.registers.LR) & ~3u;
                for (uint32_t address = callerStart; address <= cpu.registers.LR + 8; address += 4) {
                    const uint32_t word = cpu.memory.Read32(address);
                    std::cout << "0x" << std::hex << address << " 0x" << word << " "
                              << Broadway::Decoder::Format(Broadway::Decoder::Decode(word), address) << "\n";
                }
                std::cout << "Apploader setup at 0x81200294:\n";
                for (uint32_t address = 0x81200294u; address < 0x81200320u; address += 4) {
                    const uint32_t word = cpu.memory.Read32(address);
                    std::cout << "0x" << std::hex << address << " 0x" << word << " "
                              << Broadway::Decoder::Format(Broadway::Decoder::Decode(word), address) << "\n";
                }
                std::cout << "Cache helper at 0x8120115C:\n";
                for (uint32_t address = 0x8120115Cu; address < 0x812011A8u; address += 4) {
                    const uint32_t word = cpu.memory.Read32(address);
                    std::cout << "0x" << std::hex << address << " 0x" << word << " "
                              << Broadway::Decoder::Format(Broadway::Decoder::Decode(word), address) << "\n";
                }
                std::cout << "Apploader cache setup at 0x812010EC:\n";
                for (uint32_t address = 0x812010ECu; address < 0x8120115Cu; address += 4) {
                    const uint32_t word = cpu.memory.Read32(address);
                    std::cout << "0x" << std::hex << address << " 0x" << word << " "
                              << Broadway::Decoder::Format(Broadway::Decoder::Decode(word), address) << "\n";
                }
                return 6;
            }
        }

        if (probeDol) {
            const auto dolHeader = disc.ReadPartitionData(gamePartition, boot.dolOffset, 0x100, titleKey);
            uint64_t dolSize = 0x100;
            for (unsigned section = 0; section < 18; ++section) {
                const uint32_t fileOffset = (uint32_t(dolHeader[section * 4]) << 24) |
                    (uint32_t(dolHeader[section * 4 + 1]) << 16) |
                    (uint32_t(dolHeader[section * 4 + 2]) << 8) | dolHeader[section * 4 + 3];
                const size_t sizeOffset = 0x90 + section * 4;
                const uint32_t sectionSize = (uint32_t(dolHeader[sizeOffset]) << 24) |
                    (uint32_t(dolHeader[sizeOffset + 1]) << 16) |
                    (uint32_t(dolHeader[sizeOffset + 2]) << 8) | dolHeader[sizeOffset + 3];
                dolSize = std::max(dolSize, uint64_t(fileOffset) + sectionSize);
            }
            if (dolSize > 64u * 1024u * 1024u) throw std::runtime_error("DOL section table declares an implausible file size.");
            const auto dol = disc.ReadPartitionData(gamePartition, boot.dolOffset,
                                                    static_cast<size_t>(dolSize), titleKey);
            const auto loaded = Broadway::ExecutableLoader::Load(cpu, dol, Broadway::ExecutableFormat::Dol);
            gx.Reset();
            std::cout << "Direct DOL diagnostic (not a disc boot): loaded " << loaded.segmentCount
                      << " sections, entry=0x" << std::hex << loaded.entryPoint << std::dec
                      << "; apploader not run, Wii boot state not initialized.\n";
        }
        const uint64_t limit = argc == 5 ? std::stoull(argv[4]) : 1000000u;
        uint32_t lastPC = 0, lastRaw = 0;
        std::map<unsigned, uint64_t> sprAccesses;
        for (uint64_t i = 0; i < limit; ++i) {
            const uint32_t pc = cpu.registers.PC;
            const uint32_t raw = cpu.memory.Read32(pc);
            try {
                cpu.Step(); lastPC = pc; lastRaw = raw;
                const auto executed = Broadway::Decoder::Decode(raw);
                const std::string mnemonic = Broadway::Decoder::Name(executed);
                if (mnemonic == "mfspr" || mnemonic == "mtspr") {
                    const unsigned spr = ((raw >> 16) & 31u) | ((raw >> 6) & 0x3E0u);
                    ++sprAccesses[spr];
                }
            }
            catch (const Broadway::CPUException& e) {
                const auto decoded = Broadway::Decoder::Decode(raw);
                std::cout << "Stopped after " << std::dec << cpu.InstructionsExecuted()
                          << " instructions at PC=0x" << std::hex << e.FaultPC()
                          << " raw=0x" << raw << " " << Broadway::Decoder::Format(decoded, e.FaultPC())
                          << " : " << e.what() << "\n"
                          << "Last successful PC=0x" << lastPC << " raw=0x" << lastRaw
                          << " " << Broadway::Decoder::Format(Broadway::Decoder::Decode(lastRaw), lastPC) << "\n"
                          << "MSR=0x" << cpu.registers.MSR << " LR=0x" << cpu.registers.LR
                          << " CTR=0x" << cpu.registers.CTR << " CR=0x" << cpu.registers.CR
                          << " XER=0x" << cpu.registers.XER << " GX commands=" << std::dec
                          << gx.CommandsProcessed() << " XFB frames=" << gx.FramesCopied()
                          << " GX MMIO reads=" << gx.MmioReads() << " writes=" << gx.MmioWrites() << "\n";
                std::cout << "Completed SPR accesses:";
                if (sprAccesses.empty()) std::cout << " none";
                for (const auto& [spr, count] : sprAccesses)
                    std::cout << " SPR" << spr << "=" << count;
                std::cout << "\nFirst unsupported MMIO access: none observed before stop; DI is not connected.\n";
                return 5;
            }
        }
        std::cout << "Instruction limit reached: " << limit << ", PC=0x" << std::hex << cpu.registers.PC
                  << ", last=0x" << cpu.LastInstructionPC() << std::dec
                  << ", GX commands=" << gx.CommandsProcessed() << ", XFB frames=" << gx.FramesCopied() << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ISO probe failed: " << e.what() << '\n';
        return 1;
    }
}
