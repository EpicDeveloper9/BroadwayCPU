#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>
#include <functional>
#include <string>
#include <stdexcept>
#include <array>
#include <memory>
#include <unordered_map>
#include <utility>

namespace Broadway {
struct ArmElfSegment { uint32_t address=0, physicalAddress=0, fileSize=0, memorySize=0, flags=0, offset=0; };
struct ArmElfRelocation { uint32_t offset=0; uint32_t type=0; uint32_t symbol=0; int32_t addend=0; };
struct ArmElfSection { uint32_t type=0, offset=0, size=0, link=0, info=0, addr=0, entsize=0; };
struct ArmElfImage { uint16_t machine=0, type=0; uint32_t entry=0; std::vector<ArmElfSegment> segments; std::vector<ArmElfSection> sections; std::vector<ArmElfRelocation> relocations; std::vector<uint8_t> bytes; std::vector<uint8_t> bootPayload; };

class ArmElfLoader { public: static ArmElfImage Parse(const std::vector<uint8_t>&); static ArmElfImage LoadFile(const std::filesystem::path&); static ArmElfImage ParseIosBootContent(const std::vector<uint8_t>&); };

class StarletMemory {
public:
    explicit StarletMemory(size_t addressSpaceSize = size_t(0x100000000ull));
    size_t size() const noexcept { return addressSpaceSize; }
    size_t AllocatedPages() const noexcept { return pages.size(); }
    void MapRange(uint32_t address, size_t byteCount);
    bool IsMapped(uint32_t address, size_t byteCount = 1) const noexcept;
    uint8_t& operator[](size_t address);
    uint8_t operator[](size_t address) const;

private:
    static constexpr size_t kPageSize = 4096;
    size_t addressSpaceSize;
    std::vector<std::pair<uint64_t, uint64_t>> mappedRanges;
    std::unordered_map<size_t, std::unique_ptr<std::array<uint8_t, kPageSize>>> pages;
};

class StarletArm {
public:
    explicit StarletArm(size_t memorySize=size_t(0x100000000ull));
    // Current register file (active mode).
    uint32_t R[16]{};
    uint32_t CPSR=0xD3; // SVC mode, IRQ/FIQ disabled, ARM state.
    uint64_t instructions=0;
    bool halted=false;
    StarletMemory memory;

    // Minimal ARM926EJ-S CP15/MMU state used by genuine Wii IOS/boot code.
    uint32_t CP15[16]{};
    uint32_t TTBR0=0;
    uint32_t TTBR1=0;
    uint32_t DACR=0;
    uint32_t DFSR=0;
    uint32_t IFSR=0;
    bool mmuEnabled=false;
    bool alignmentFaultsEnabled=true;
    uint32_t lastFaultAddress=0;

    // Memory access (big-endian ARM view).
    uint8_t Read8(uint32_t address) const;
    uint16_t Read16(uint32_t address) const;
    uint32_t Read32(uint32_t address) const;
    void Write8(uint32_t address,uint8_t value);
    void Write16(uint32_t address,uint16_t value);
    void Write32(uint32_t address,uint32_t value);
    bool InMemory(uint32_t address,size_t size=1) const;

    // MMIO hooks for addresses outside RAM (optional).
    std::function<uint32_t(uint32_t)> mmioRead;
    std::function<void(uint32_t,uint32_t)> mmioWrite;
    std::function<uint32_t(uint32_t,unsigned)> mmioReadAccess;
    std::function<void(uint32_t,unsigned,uint32_t)> mmioWriteAccess;

    // Execution.
    void Load(const ArmElfImage& image);
    void LoadBinary(const std::vector<uint8_t>& bytes,uint32_t address);
    void Step();
    void Run(uint64_t limit);

    // Mode/register helpers.
    uint32_t Mode() const { return CPSR & 0x1Fu; }
    bool Thumb() const { return (CPSR & 0x20u)!=0; }
    uint32_t SPSR() const;
    void SetSPSR(uint32_t value);
    void ChangeMode(uint32_t newMode);
    void RaiseException(uint32_t vectorOffset,uint32_t newMode);
    uint32_t BankedRegister(int reg,uint32_t mode) const;
    void SetBankedRegister(int reg,uint32_t mode,uint32_t value);
    void SetCp15Register(uint32_t reg, uint32_t value);
    uint32_t GetCp15Register(uint32_t reg) const;
    uint32_t TranslateVirtualAddress(uint32_t addr) const;
    uint32_t TranslateForAccess(uint32_t addr) const;

    // Flags.
    bool FlagN() const { return (CPSR & (1u<<31))!=0; }
    bool FlagZ() const { return (CPSR & (1u<<30))!=0; }
    bool FlagC() const { return (CPSR & (1u<<29))!=0; }
    bool FlagV() const { return (CPSR & (1u<<28))!=0; }
    void SetFlags(bool n,bool z,bool c,bool v);

    // Public for tests/inspection.
    uint64_t InstructionCount() const { return instructions; }

private:
    // Banked storage. Index by mode number mapping.
    int ModeIndex(uint32_t mode) const;
    uint32_t bankedR[7][16]{}; // 0 user,1 fiq,2 irq,3 svc,4 abort,5 undef,6 system (system shares user except SPSR)
    uint32_t bankedSPSR[7]{};
    int currentModeIndex=3; // SVC

    void SaveBanked(int modeIdx);
    void RestoreBanked(int modeIdx, int oldModeIdx);
    void SyncCurrentToBanked();
    void SyncBankedToCurrent();

    // ALU
    uint32_t Reg(int r) const { return (r==15) ? (Thumb()?R[15]+2:R[15]+4) : R[r]; } // read PC with offset? careful
    // We'll handle PC reads explicitly in decode.

    // Helpers for condition
    bool ConditionPassed(uint32_t cond) const;

    // ARM decode
    void StepArm();
    void StepThumb();

    // Memory helpers for MMIO
    uint32_t ReadMemory(uint32_t address,size_t size) const;
    void WriteMemory(uint32_t address,uint32_t value,size_t size);
};
}
