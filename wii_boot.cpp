#include "wii_boot.hpp"

#include "cpu.hpp"
#include "gx.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace Broadway {
namespace {
constexpr uint32_t kApploaderLoadAddress = 0x81200000u;
constexpr uint32_t kReturnSentinel = 0x80003020u;
constexpr uint32_t kReportCallback = 0x80003000u;
constexpr uint32_t kFunctionSlots = 0x80003100u;
constexpr uint32_t kRequestSlots = 0x80003110u;
constexpr uint32_t kStackTop = 0x817ff000u;
constexpr uint32_t kBlr = 0x4E800020u;
constexpr uint32_t kMem1Size = 0x01800000u;

uint32_t BE32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
        (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void CopyToMemory(Memory& memory, uint32_t address, const std::vector<uint8_t>& bytes) {
    memory.Validate(address, bytes.size());
    for (size_t i = 0; i < bytes.size(); ++i)
        memory.Write8(address + uint32_t(i), bytes[i]);
}
}

void InitializeWiiBootMemory(Memory& memory,
                             const std::array<uint8_t, 0x20>& discHeader,
                             uint64_t partitionOffsetBytes,
                             uint32_t partitionType) {
    CopyToMemory(memory, 0x80000000u,
                 std::vector<uint8_t>(discHeader.begin(), discHeader.end()));
    memory.Write32(0x80000020u, 0x0D15EA5Eu); // Standard Wii/GameCube disc boot marker.
    memory.Write32(0x80000024u, 1u);          // Apploader ABI version.
    memory.Write32(0x80000028u, kMem1Size);   // Physical MEM1 size.
    memory.Write32(0x8000002Cu, 0x23u);       // Wii production board model.
    memory.Write32(0x800000F0u, kMem1Size);   // Simulated MEM1 size.
    memory.Write32(0x800000F8u, 0x0E7BE2C0u);// Broadway bus clock (243 MHz).
    memory.Write32(0x800000FCu, 0x2B73A840u);// Broadway CPU clock (729 MHz).

    memory.Write32(0x80003100u, kMem1Size);   // Physical MEM1 size.
    memory.Write32(0x80003104u, kMem1Size);   // Simulated MEM1 size.
    memory.Write8(0x8000315Cu, 0x80u);       // Started by the Wii boot program.
    memory.Write8(0x80003184u, 0x80u);       // Disc application type.
    memory.Write32(0x80003194u, partitionType);
    if (partitionOffsetBytes > UINT32_MAX)
        throw std::runtime_error("Selected partition offset does not fit Wii low memory.");
    memory.Write32(0x80003198u, static_cast<uint32_t>(partitionOffsetBytes));
}

WiiBootSession::WiiBootSession(const std::filesystem::path& imagePath,
                               const std::array<uint8_t, 16>& commonKey)
    : disc(imagePath) {
    if (!disc.Header().encrypted)
        throw std::runtime_error("This boot path currently expects an encrypted retail Wii partition.");
    for (size_t i = 0; i < disc.Partitions().size(); ++i) {
        if (disc.Partitions()[i].type == 0) { gamePartition = i; break; }
    }
    if (disc.Partitions()[gamePartition].type != 0)
        throw std::runtime_error("No Wii game data partition was found.");
    titleKey = disc.DeriveTitleKey(gamePartition, commonKey);
    boot = disc.ReadBootInformation(gamePartition, titleKey);
    if (boot.apploaderEntry < kApploaderLoadAddress ||
        uint64_t(boot.apploaderEntry) >= uint64_t(kApploaderLoadAddress) +
            boot.apploaderCodeSize + boot.apploaderTrailerSize)
        throw std::runtime_error("Apploader entry point is outside its loaded image.");
}

void WiiBootSession::Call(CPU& cpu, uint32_t function, uint32_t r3, uint32_t r4,
                          uint32_t r5, uint64_t instructionLimit) {
    if ((function & 3u) != 0) throw std::runtime_error("Apploader returned an unaligned function pointer.");
    cpu.registers.PC = function;
    cpu.registers.LR = kReturnSentinel;
    cpu.registers.GPR[3] = r3;
    cpu.registers.GPR[4] = r4;
    cpu.registers.GPR[5] = r5;
    for (uint64_t i = 0; i < instructionLimit; ++i) {
        if (cpu.registers.PC == kReturnSentinel) return;
        cpu.Step();
    }
    throw std::runtime_error("Apploader routine exceeded its instruction limit.");
}

WiiBootResult WiiBootSession::Boot(CPU& cpu, GXProcessor& gx) {
    cpu.memory.Clear();
    cpu.Reset();
    gx.Reset();

    // Recreate the documented disc/boot fields written during the Wii
    // System Menu to apploader handoff. OS exception vectors and OSContext
    // globals deliberately remain absent; they belong to Revolution OS.
    std::array<uint8_t, 0x20> discHeader{};
    const auto headerBytes = disc.ReadRaw(0, discHeader.size());
    std::copy(headerBytes.begin(), headerBytes.end(), discHeader.begin());
    const auto& gamePartitionInfo = disc.Partitions()[gamePartition];
    InitializeWiiBootMemory(cpu.memory, discHeader, gamePartitionInfo.offset,
                            gamePartitionInfo.type);

    cpu.memory.Write32(kReportCallback, kBlr);
    cpu.memory.Write32(kReturnSentinel, kBlr);
    cpu.registers.GPR[1] = kStackTop;
    cpu.registers.GPR[2] = 0x80400000u;
    cpu.registers.GPR[13] = 0x80000000u;
    cpu.registers.MSR = CPURegisters::MSR_FP;

    const auto apploader = disc.ReadApploaderImage(gamePartition, boot, titleKey);
    CopyToMemory(cpu.memory, kApploaderLoadAddress, apploader);
    cpu.registers.PC = boot.apploaderEntry;

    Call(cpu, boot.apploaderEntry, kFunctionSlots, kFunctionSlots + 4,
         kFunctionSlots + 8, 10000000);
    const uint32_t init = cpu.memory.Read32(kFunctionSlots);
    const uint32_t main = cpu.memory.Read32(kFunctionSlots + 4);
    const uint32_t close = cpu.memory.Read32(kFunctionSlots + 8);
    if (!init || !main || !close)
        throw std::runtime_error("Apploader entry did not return all three callback addresses.");

    Call(cpu, init, kReportCallback, 0, 0, 10000000);

    WiiBootResult result;
    result.gameId = disc.Header().gameId;
    result.revision = disc.Header().revision;
    result.apploaderEntry = boot.apploaderEntry;
    uint32_t fstAddress = 0;
    for (uint64_t request = 0; request < 100000; ++request) {
        Call(cpu, main, kRequestSlots, kRequestSlots + 4, kRequestSlots + 8, 10000000);
        if (cpu.registers.GPR[3] == 0) break;

        const uint32_t destination = cpu.memory.Read32(kRequestSlots);
        const uint32_t length = cpu.memory.Read32(kRequestSlots + 4);
        const uint32_t offset = cpu.memory.Read32(kRequestSlots + 8);
        if (length == 0) throw std::runtime_error("Apploader requested a zero-length disc read.");
        cpu.memory.Validate(destination, length);
        const auto bytes = disc.ReadPartitionData(gamePartition, offset, length, titleKey);
        CopyToMemory(cpu.memory, destination, bytes);
        ++result.discReadRequests;
        result.discBytesLoaded += length;
        if (offset <= boot.fstOffset &&
            uint64_t(offset) + length >= uint64_t(boot.fstOffset) + boot.fstSize)
            fstAddress = destination + (boot.fstOffset - offset);
    }
    if (result.discReadRequests == 100000)
        throw std::runtime_error("Apploader did not finish within the disc-read request limit.");

    Call(cpu, close, 0, 0, 0, 10000000);
    result.gameEntry = cpu.registers.GPR[3];
    if ((result.gameEntry & 3u) != 0)
        throw std::runtime_error("Apploader returned an unaligned game entry point.");

    const auto dolHeader = disc.ReadPartitionData(gamePartition, boot.dolOffset, 0x100, titleKey);
    const uint32_t expectedEntry = BE32(dolHeader.data() + 0xE0);
    if (result.gameEntry != expectedEntry)
        throw std::runtime_error("Apploader close entry point does not match the partition DOL header.");
    const uint32_t bssAddress = BE32(dolHeader.data() + 0xD8);
    const uint32_t bssSize = BE32(dolHeader.data() + 0xDC);
    if (bssSize) {
        cpu.memory.Validate(bssAddress, bssSize);
        for (uint32_t i = 0; i < bssSize; ++i)
            cpu.memory.Write8(bssAddress + i, 0);
    }
    if (fstAddress) {
        cpu.memory.Write32(0x80000038u, fstAddress);
        cpu.memory.Write32(0x8000003Cu, boot.fstSize);
    }
    cpu.registers.PC = result.gameEntry;
    result.instructionsExecuted = cpu.InstructionsExecuted();
    return result;
}

} // namespace Broadway
