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
    void Reset();
};
}
