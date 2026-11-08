#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>

namespace Broadway {
struct CPURegisters {
    std::array<uint32_t, 32> GPR{};
    std::array<double, 32> FPR{};

    uint32_t PC = 0;
    uint32_t LR = 0;
    uint32_t CTR = 0;
    uint32_t XER = 0;
    uint32_t CR = 0;
    uint32_t MSR = 0;
    uint32_t FPSCR = 0;
    uint32_t HID2 = 0;
    uint32_t HID4 = 0; // SPR 1011 Gekko/Broadway hardware implementation control state.
    uint32_t HID0 = 0;
    uint32_t HID1 = 0;
    uint32_t WPAR = 0; // SPR 921; bit 0 reports write-gather buffer non-empty.
    uint32_t L2CR = 0; // SPR 1017 configuration state; cache contents/timing are not modeled.
    uint32_t MMCR0 = 0; // SPR 952 performance-monitor control; PMC event counting is not modeled.
    uint32_t MMCR1 = 0; // SPR 956 event selectors; performance-monitor counters are not modeled.
    std::array<uint32_t, 4> PMC{}; // SPRs 953/954/957/958; event-driven increments are not modeled.
    uint32_t DAR = 0;
    uint32_t DSISR = 0;
    uint32_t SDR1 = 0;
    uint32_t DEC = 0;
    std::array<uint32_t, 8> GQR{};
    std::array<uint32_t, 16> SR{};
    std::array<uint32_t, 4> SPRG{};
    std::array<std::array<uint32_t, 2>, 4> IBAT{};
    std::array<std::array<uint32_t, 2>, 4> DBAT{};
    uint64_t TimeBase = 0; // Clocked by the surrounding emulated system.
    uint32_t SRR0 = 0;
    uint32_t SRR1 = 0;

    static constexpr uint32_t XER_SO = 1u << 31;
    static constexpr uint32_t XER_OV = 1u << 30;
    static constexpr uint32_t XER_CA = 1u << 29;

    uint32_t GetCRField(unsigned field) const {
        if (field >= 8) throw std::out_of_range("CR field index must be 0..7");
        return (CR >> (28u - field * 4u)) & 0xFu;
    }
    void SetCRField(unsigned field, uint32_t value) {
        if (field >= 8) throw std::out_of_range("CR field index must be 0..7");
        const uint32_t shift = 28u - field * 4u;
        CR = (CR & ~(0xFu << shift)) | ((value & 0xFu) << shift);
    }
    bool GetXERCarry() const { return (XER & XER_CA) != 0; }
    bool GetXEROverflow() const { return (XER & XER_OV) != 0; }
    bool GetXERSummaryOverflow() const { return (XER & XER_SO) != 0; }
    void SetXERCarry(bool value) {
        if (value) XER |= XER_CA;
        else XER &= ~XER_CA;
    }
    void SetXEROverflow(bool value) {
        if (value) { XER |= XER_OV | XER_SO; }
        else { XER &= ~XER_OV; }
    }
    static constexpr uint32_t HID2_PSE = 1u << 29;
    static constexpr uint32_t HID2_LSQE = 1u << 31;
    static constexpr uint32_t MSR_PR = 1u << 17;
    static constexpr uint32_t MSR_FP = 1u << 13;

    void Reset();
};
}
