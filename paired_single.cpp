#include "cpu.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Broadway {
namespace {
struct Pair { float ps0; float ps1; };

Pair GetPair(double value) {
    const uint64_t bits = std::bit_cast<uint64_t>(value);
    return {std::bit_cast<float>(uint32_t(bits >> 32)), std::bit_cast<float>(uint32_t(bits))};
}

double PutPair(float ps0, float ps1) {
    const uint64_t bits = (uint64_t(std::bit_cast<uint32_t>(ps0)) << 32) |
        std::bit_cast<uint32_t>(ps1);
    return std::bit_cast<double>(bits);
}

int SignedScale(uint32_t value) {
    value &= 0x3Fu;
    return (value & 0x20u) ? int(value) - 64 : int(value);
}

uint32_t ReadQuantized(CPU& cpu, uint32_t address, unsigned type, uint32_t pc) {
    try {
        switch (type) {
        case 0:
        case 5:
        case 7:
            if ((address & 1u) != 0) throw CPUException(ExceptionKind::Alignment, pc, "Unaligned 16-bit paired-single load");
            return cpu.memory.Read16(address);
        case 4:
        case 6:
            return cpu.memory.Read8(address);
        default:
            throw CPUException(ExceptionKind::InvalidInstruction, pc, "Reserved GQR load type");
        }
    } catch (const CPUException&) {
        throw;
    } catch (const std::exception&) {
        throw CPUException(ExceptionKind::DataStorage, pc, "Paired-single load outside mapped memory");
    }
}

float Dequantize(CPU& cpu, uint32_t address, unsigned type, int scale, uint32_t pc) {
    if (type == 0) {
        if (address & 3u) throw CPUException(ExceptionKind::Alignment, pc, "Unaligned paired-single float load");
        try { return std::bit_cast<float>(cpu.memory.Read32(address)); }
        catch (const std::exception&) { throw CPUException(ExceptionKind::DataStorage, pc, "Paired-single load outside mapped memory"); }
    }

    const uint32_t raw = ReadQuantized(cpu, address, type, pc);
    int32_t integer = 0;
    switch (type) {
    case 4: integer = int32_t(raw); break;
    case 5: integer = int32_t(uint16_t(raw)); break;
    case 6: integer = int8_t(raw); break;
    case 7: integer = int16_t(raw); break;
    default: throw CPUException(ExceptionKind::InvalidInstruction, pc, "Reserved GQR load type");
    }
    return static_cast<float>(std::ldexp(static_cast<double>(integer), -scale));
}

uint32_t Quantize(float value, unsigned type, int scale, uint32_t pc) {
    if (type == 0) {
        if (std::fpclassify(value) == FP_SUBNORMAL) value = 0.0f;
        return std::bit_cast<uint32_t>(value);
    }

    long double scaled;
    if (std::isnan(value)) scaled = std::numeric_limits<long double>::infinity();
    else scaled = std::ldexp(static_cast<long double>(value), scale);

    const bool unsignedType = type == 4 || type == 5;
    const bool eightBit = type == 4 || type == 6;
    if (type < 4 || type > 7)
        throw CPUException(ExceptionKind::InvalidInstruction, pc, "Reserved GQR store type");

    long double minimum = 0.0L;
    long double maximum = eightBit ? 255.0L : 65535.0L;
    if (!unsignedType) {
        minimum = eightBit ? -128.0L : -32768.0L;
        maximum = eightBit ? 127.0L : 32767.0L;
    }
    if (std::isinf(scaled)) {
        if (scaled < 0 && unsignedType) return 0;
        return static_cast<uint32_t>(scaled < 0 ? minimum : maximum);
    }
    if (unsignedType && scaled < 0.0L) return 0;
    scaled = std::trunc(scaled);
    if (scaled < minimum) scaled = minimum;
    if (scaled > maximum) scaled = maximum;
    if (type == 6) return uint8_t(static_cast<int8_t>(scaled));
    if (type == 7) return uint16_t(static_cast<int16_t>(scaled));
    return static_cast<uint32_t>(scaled);
}

void StoreQuantized(CPU& cpu, uint32_t address, uint32_t value, unsigned type, uint32_t pc) {
    try {
        if (type == 0) {
            if (address & 3u) throw CPUException(ExceptionKind::Alignment, pc, "Unaligned paired-single float store");
            cpu.memory.Write32(address, value);
        } else if (type == 4 || type == 6) {
            cpu.memory.Write8(address, uint8_t(value));
        } else if (type == 5 || type == 7) {
            if (address & 1u) throw CPUException(ExceptionKind::Alignment, pc, "Unaligned 16-bit paired-single store");
            cpu.memory.Write16(address, uint16_t(value));
        } else {
            throw CPUException(ExceptionKind::InvalidInstruction, pc, "Reserved GQR store type");
        }
    } catch (const CPUException&) {
        throw;
    } catch (const std::exception&) {
        throw CPUException(ExceptionKind::DataStorage, pc, "Paired-single store outside mapped memory");
    }
}
}

void CPU::ExecutePairedSingle(const Instruction& ins) {
    if ((registers.MSR & CPURegisters::MSR_FP) == 0)
        throw CPUException(ExceptionKind::FloatingPointUnavailable,lastInstructionPC,
                           "Paired-single instruction while MSR[FP] is clear");
    const uint32_t raw = ins.raw;
    const unsigned primary = raw >> 26;
    const unsigned frt = (raw >> 21) & 31u;
    const unsigned fra = (raw >> 16) & 31u;
    const unsigned frb = (raw >> 11) & 31u;
    const unsigned frc = (raw >> 6) & 31u;
    auto& f = registers.FPR;

    if ((registers.HID2 & CPURegisters::HID2_PSE) == 0)
        throw CPUException(ExceptionKind::InvalidInstruction, lastInstructionPC,
                           "Paired-single instruction requires HID2[PSE]");

    if (primary == 56 || primary == 57 || primary == 60 || primary == 61) {
        const bool store = primary >= 60;
        const bool update = (primary & 1u) != 0;
        if ((registers.HID2 & CPURegisters::HID2_LSQE) == 0)
            throw CPUException(ExceptionKind::InvalidInstruction, lastInstructionPC,
                               "Non-indexed quantized load/store requires HID2[LSQE]");
        if (update && fra == 0)
            throw CPUException(ExceptionKind::InvalidInstruction, lastInstructionPC,
                               "Paired-single update form requires nonzero RA");

        const int32_t displacement = (raw & 0x800u) ? int32_t(raw & 0xFFFu) - 0x1000 : int32_t(raw & 0xFFFu);
        const uint32_t address = (fra ? registers.GPR[fra] : 0u) + uint32_t(displacement);
        const bool oneElement = ((raw >> 15) & 1u) != 0;
        const unsigned gqrIndex = (raw >> 12) & 7u;
        const uint32_t gqr = registers.GQR[gqrIndex];
        const unsigned type = store ? (gqr & 7u) : ((gqr >> 16) & 7u);
        const int scale = SignedScale(store ? ((gqr >> 8) & 0x3Fu) : ((gqr >> 24) & 0x3Fu));

        if (store) {
            const Pair pair = GetPair(f[frt]);
            StoreQuantized(*this, address, Quantize(pair.ps0, type, scale, lastInstructionPC), type, lastInstructionPC);
            if (!oneElement) {
                const unsigned size = type == 0 ? 4u : (type == 5 || type == 7 ? 2u : 1u);
                StoreQuantized(*this, address + size, Quantize(pair.ps1, type, scale, lastInstructionPC), type, lastInstructionPC);
            }
        } else {
            const unsigned size = type == 0 ? 4u : (type == 5 || type == 7 ? 2u : 1u);
            const float ps0 = Dequantize(*this, address, type, scale, lastInstructionPC);
            const float ps1 = oneElement ? 1.0f : Dequantize(*this, address + size, type, scale, lastInstructionPC);
            f[frt] = PutPair(ps0, ps1);
        }
        if (update) registers.GPR[fra] = address;
        return;
    }

    const unsigned xo5 = (raw >> 1) & 31u;
    const bool aForm = xo5 == 10 || xo5 == 11 || xo5 == 12 || xo5 == 13 || xo5 == 14 || xo5 == 15 ||
        xo5 == 18 || xo5 == 20 || xo5 == 21 || xo5 == 23 || xo5 == 25 ||
        xo5 == 28 || xo5 == 29 || xo5 == 30 || xo5 == 31;
    const unsigned xo = aForm ? xo5 : ((raw >> 1) & 0x3FFu);
    const Pair a = GetPair(f[fra]);
    const Pair b = GetPair(f[frb]);
    const Pair c = GetPair(f[frc]);
    float ps0 = 0.0f, ps1 = 0.0f;
    switch (xo) {
    case 0: case 32: case 64: case 96: { // ps_cmp{u,o}{0,1}
        const bool lane1 = xo == 64 || xo == 96;
        const float lhs = lane1 ? a.ps1 : a.ps0;
        const float rhs = lane1 ? b.ps1 : b.ps0;
        uint32_t field = 2u;
        if (std::isnan(lhs) || std::isnan(rhs)) field = 1u;
        else if (lhs < rhs) field = 8u;
        else if (lhs > rhs) field = 4u;
        if (registers.GetXERSummaryOverflow()) field |= 1u;
        registers.SetCRField((raw >> 23) & 7u, field);
        return;
    }
    case 18: ps0 = a.ps0 / b.ps0; ps1 = a.ps1 / b.ps1; break;
    case 20: ps0 = a.ps0 - b.ps0; ps1 = a.ps1 - b.ps1; break;
    case 21: ps0 = a.ps0 + b.ps0; ps1 = a.ps1 + b.ps1; break;
    case 23: ps0 = !std::signbit(a.ps0) ? c.ps0 : b.ps0; ps1 = !std::signbit(a.ps1) ? c.ps1 : b.ps1; break;
    case 25: ps0 = a.ps0 * c.ps0; ps1 = a.ps1 * c.ps1; break;
    case 28: ps0 = std::fma(a.ps0, c.ps0, -b.ps0); ps1 = std::fma(a.ps1, c.ps1, -b.ps1); break;
    case 29: ps0 = std::fma(a.ps0, c.ps0, b.ps0); ps1 = std::fma(a.ps1, c.ps1, b.ps1); break;
    case 30: ps0 = -std::fma(a.ps0, c.ps0, -b.ps0); ps1 = -std::fma(a.ps1, c.ps1, -b.ps1); break;
    case 31: ps0 = -std::fma(a.ps0, c.ps0, b.ps0); ps1 = -std::fma(a.ps1, c.ps1, b.ps1); break;
    case 10: ps0 = a.ps0 + b.ps1; ps1 = c.ps1; break;
    case 11: ps0 = c.ps0; ps1 = a.ps0 + b.ps1; break;
    case 12: ps0 = a.ps0 * c.ps0; ps1 = a.ps1 * c.ps0; break;
    case 13: ps0 = a.ps0 * c.ps1; ps1 = a.ps1 * c.ps1; break;
    case 14: ps0 = std::fma(a.ps0, c.ps0, b.ps0); ps1 = std::fma(a.ps1, c.ps0, b.ps1); break;
    case 15: ps0 = std::fma(a.ps0, c.ps1, b.ps0); ps1 = std::fma(a.ps1, c.ps1, b.ps1); break;
    case 40: ps0 = -a.ps0; ps1 = -a.ps1; break;
    case 72: ps0 = a.ps0; ps1 = a.ps1; break;
    case 136: ps0 = std::fabs(a.ps0); ps1 = std::fabs(a.ps1); break;
    case 264: ps0 = -std::fabs(a.ps0); ps1 = -std::fabs(a.ps1); break;
    case 528: ps0 = a.ps0; ps1 = b.ps0; break;
    case 560: ps0 = a.ps0; ps1 = b.ps1; break;
    case 592: ps0 = a.ps1; ps1 = b.ps0; break;
    case 624: ps0 = a.ps1; ps1 = b.ps1; break;
    default:
        throw CPUException(ExceptionKind::UnimplementedInstruction, lastInstructionPC,
                           "Unsupported paired-single instruction");
    }

    f[frt] = PutPair(ps0, ps1);
    if (raw & 1u) UpdateCR1();
}
}
