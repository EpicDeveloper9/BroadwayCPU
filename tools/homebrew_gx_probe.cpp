#include "../cpu.hpp"
#include "../executable_loader.hpp"
#include "../gx.hpp"
#include "../exi.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
uint64_t ParseInstructionLimit(const char* text) {
    size_t parsed = 0;
    const unsigned long long value = std::stoull(text, &parsed, 0);
    if (text[parsed] != '\0' || value == 0)
        throw std::runtime_error("Instruction limit must be a positive integer.");
    return value;
}
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: homebrew_gx_probe <homebrew.elf|homebrew.dol> [instruction-limit]\n";
        return 2;
    }

    try {
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("Could not open the homebrew executable.");
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        const uint64_t instructionLimit = argc == 3
            ? ParseInstructionLimit(argv[2]) : 10000000u;

        Broadway::CPU cpu(32u * 1024u * 1024u);
        Broadway::VideoFrameMailbox mailbox;
        Broadway::GXProcessor gx(cpu.memory, mailbox);
        Broadway::EXIController exi;
        cpu.memory.AttachMmioDevice(&gx);
        cpu.memory.AddMmioDevice(&exi);

        const auto extension = std::filesystem::path(argv[1]).extension();
        const auto format = extension == ".elf" || extension == ".ELF"
            ? Broadway::ExecutableFormat::Elf
            : Broadway::ExecutableFormat::Dol;
        const auto loaded = Broadway::ExecutableLoader::Load(cpu, bytes, format);
        cpu.registers.MSR = Broadway::CPURegisters::MSR_FP;
        std::cout << "Loaded " << loaded.formatName << " entry=0x" << std::hex
                  << loaded.entryPoint << " segments=" << std::dec << loaded.segmentCount
                  << " instruction_limit=" << instructionLimit
                  << " initial_MSR=0x" << std::hex << cpu.registers.MSR << '\n';

        uint64_t previousExiReads = 0;
        uint64_t previousExiWrites = 0;
        uint64_t previousGxWrites = 0;
        uint64_t previousGxCommands = 0;
        uint64_t previousPrimitives = 0;
        uint64_t previousFrames = 0;
        bool stoppedOnException = false;
        for (uint64_t i = 0; i < instructionLimit; ++i) {
            const uint32_t previousMsr = cpu.registers.MSR;
            try {
                cpu.Step();
            } catch (const std::exception& e) {
                stoppedOnException = true;
                std::cout << "CPU stopped at PC=0x" << std::hex << cpu.LastInstructionPC()
                          << ": " << e.what() << '\n';
                break;
            }
            if (cpu.registers.MSR != previousMsr) {
                std::cout << "MSR change at PC=0x" << std::hex << cpu.LastInstructionPC()
                          << " 0x" << previousMsr << "->0x" << cpu.registers.MSR << '\n';
            }

            if (exi.MmioReads() != previousExiReads ||
                exi.MmioWrites() != previousExiWrites) {
                if (previousExiReads == 0 && previousExiWrites == 0) {
                    std::cout << "First EXI activity after instruction " << std::dec
                              << cpu.InstructionsExecuted() << " at PC=0x" << std::hex
                              << cpu.LastInstructionPC() << " CSR0=0x" << exi.Status(0)
                              << " CR0=0x" << exi.Control(0) << '\n';
                }
                previousExiReads = exi.MmioReads();
                previousExiWrites = exi.MmioWrites();
            }

            if (gx.MmioWrites() != previousGxWrites ||
                gx.CommandsProcessed() != previousGxCommands) {
                std::cout << "GX activity at instruction " << std::dec
                          << cpu.InstructionsExecuted() << " PC=0x" << std::hex
                          << cpu.LastInstructionPC() << " MMIO_writes=" << std::dec
                          << gx.MmioWrites() << " commands=" << gx.CommandsProcessed();
                if (gx.IsStalled()) std::cout << " stalled: " << gx.LastError();
                std::cout << '\n';
                previousGxWrites = gx.MmioWrites();
                previousGxCommands = gx.CommandsProcessed();
            }
            if (gx.PrimitivesSubmitted() != previousPrimitives) {
                if (!previousPrimitives)
                    std::cout << "First GX primitive at instruction " << std::dec
                              << cpu.InstructionsExecuted() << " PC=0x" << std::hex
                              << cpu.LastInstructionPC() << '\n';
                previousPrimitives = gx.PrimitivesSubmitted();
            }
            if (gx.FramesCopied() != previousFrames) {
                if (!previousFrames)
                    std::cout << "First GX frame copy at instruction " << std::dec
                              << cpu.InstructionsExecuted() << " PC=0x" << std::hex
                              << cpu.LastInstructionPC() << '\n';
                previousFrames = gx.FramesCopied();
            }
            if (gx.IsStalled()) break;
        }

        const uint32_t pc = stoppedOnException ? cpu.LastInstructionPC() : cpu.registers.PC;
        uint32_t raw = 0;
        try { raw = cpu.memory.Read32(pc); } catch (...) {}
        const auto decoded = Broadway::Decoder::Decode(raw);
        std::cout << "Final instructions=" << std::dec << cpu.InstructionsExecuted()
                  << " PC=0x" << std::hex << cpu.registers.PC
                  << " last=0x" << cpu.LastInstructionPC()
                  << " raw=0x" << raw << " "
                  << Broadway::Decoder::Format(decoded, pc) << '\n'
                  << "GPR r1=0x" << cpu.registers.GPR[1]
                  << " r2=0x" << cpu.registers.GPR[2]
                  << " r3=0x" << cpu.registers.GPR[3]
                  << " r9=0x" << cpu.registers.GPR[9]
                  << " r10=0x" << cpu.registers.GPR[10]
                  << " r23=0x" << cpu.registers.GPR[23]
                  << " r30=0x" << cpu.registers.GPR[30]
                  << " r31=0x" << cpu.registers.GPR[31]
                  << " LR=0x" << cpu.registers.LR
                  << " MSR=0x" << cpu.registers.MSR << '\n'
                  << "EXI reads=" << std::dec << exi.MmioReads()
                  << " writes=" << exi.MmioWrites()
                  << " start_requests=" << exi.TransferStartRequests()
                  << " completed=" << exi.TransfersCompleted()
                  << " CSR0=0x" << std::hex << exi.Status(0)
                  << " CR0=0x" << exi.Control(0)
                  << " immediate0=0x" << exi.ImmediateData(0) << '\n'
                  << "GX MMIO_reads=" << std::dec << gx.MmioReads()
                  << " MMIO_writes=" << gx.MmioWrites()
                  << " commands=" << gx.CommandsProcessed()
                  << " primitives=" << gx.PrimitivesSubmitted()
                  << " efb_pixels=" << gx.EfbPixelsWritten()
                  << " frames_copied=" << gx.FramesCopied()
                  << " stalled=" << (gx.IsStalled() ? "yes" : "no")
                  << " fifo_pending=" << gx.PendingFifoBytes()
                  << "/" << gx.ExpectedFifoBytes() << '\n';
        std::cout << "GX BP scissor=0x" << std::hex << gx.BpRegisterValue(0x20)
                  << "/0x" << gx.BpRegisterValue(0x21) << '\n';
        std::cout << "GX BP copy=" << std::hex
                  << gx.BpRegisterValue(0x49) << ' ' << gx.BpRegisterValue(0x4A) << ' '
                  << gx.BpRegisterValue(0x4B) << ' ' << gx.BpRegisterValue(0x4D) << ' '
                  << gx.BpRegisterValue(0x4E) << ' ' << gx.BpRegisterValue(0x52) << '\n';
        const auto frame = mailbox.TakeLatest();
        std::cout << "Published_frame=" << (frame ? "yes" : "no");
        if (frame)
            std::cout << " size=" << std::dec << frame->Width() << 'x' << frame->Height();
        if (frame && frame->Format() == Broadway::VideoPixelFormat::YUYV422BT601Limited) {
            uint8_t minLuma = 255;
            uint8_t maxLuma = 0;
            for (uint32_t y = 0; y < frame->Height(); ++y) {
                for (uint32_t x = 0; x < frame->Width(); x += 2) {
                    const size_t offset = size_t(y) * frame->RowPitch() + size_t(x) * 2;
                    minLuma = std::min(minLuma, frame->Data()[offset]);
                    maxLuma = std::max(maxLuma, frame->Data()[offset]);
                    maxLuma = std::max(maxLuma, frame->Data()[offset + 2]);
                }
            }
            const size_t center = size_t(frame->Height() / 2) * frame->RowPitch() +
                                  size_t(frame->Width() / 2) * 2;
            std::cout << " luma_min=" << unsigned(minLuma)
                      << " luma_max=" << unsigned(maxLuma)
                      << " center_y=" << unsigned(frame->Data()[center]);
        }
        std::cout << '\n';
        if (gx.IsStalled()) {
            std::cout << "GX VCD_LO=0x" << std::hex << gx.CpRegisterValue(0x50)
                      << " VCD_HI=0x" << gx.CpRegisterValue(0x60)
                      << " VAT_A=0x" << gx.CpRegisterValue(0x70)
                      << " VAT_B=0x" << gx.CpRegisterValue(0x80)
                      << " VAT_C=0x" << gx.CpRegisterValue(0x90) << '\n';
            std::cout << "GX XF pos=";
            for (uint16_t i = 0; i < 12; ++i)
                std::cout << std::hex << gx.XfRegisterValue(i) << ' ';
            std::cout << " viewport=";
            for (uint16_t i = 0x101A; i <= 0x101F; ++i)
                std::cout << std::hex << gx.XfRegisterValue(i) << ' ';
            std::cout << " projection=";
            for (uint16_t i = 0x1020; i <= 0x1026; ++i)
                std::cout << std::hex << gx.XfRegisterValue(i) << ' ';
            std::cout << '\n';
            std::cout << "GX FIFO bytes=" << std::hex << std::setfill('0');
            for (const uint8_t byte : gx.FifoTrace())
                std::cout << std::setw(2) << unsigned(byte) << ' ';
            std::cout << std::dec << '\n';
        }
        if (gx.IsStalled()) return 1;
        return stoppedOnException ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << "Homebrew probe failed: " << e.what() << '\n';
        return 1;
    }
}
