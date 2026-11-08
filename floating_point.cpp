#include "cpu.hpp"
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Broadway {
namespace {
uint32_t FBits(uint32_t raw, unsigned shift) { return (raw >> shift) & 31u; }
float ToSingle(double x) { return static_cast<float>(x); }
bool PairedSingleEnabled(const CPURegisters& registers) {
    return (registers.HID2 & CPURegisters::HID2_PSE) != 0;
}
float GetPS0(double value) {
    return std::bit_cast<float>(uint32_t(std::bit_cast<uint64_t>(value) >> 32));
}
double PutPair(float ps0, float ps1) {
    const uint64_t bits = (uint64_t(std::bit_cast<uint32_t>(ps0)) << 32) |
        std::bit_cast<uint32_t>(ps1);
    return std::bit_cast<double>(bits);
}
int32_t ConvertToInteger(double value, bool truncate, uint32_t fpscr) {
    if (!std::isfinite(value)) return std::numeric_limits<int32_t>::min();
    double rounded = value;
    if (truncate) {
        rounded = std::trunc(value);
    } else {
        switch (fpscr & 3u) {
        case 0: rounded = std::nearbyint(value); break;
        case 1: rounded = std::trunc(value); break;
        case 2: rounded = std::ceil(value); break;
        case 3: rounded = std::floor(value); break;
        }
    }
    if (rounded < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
        rounded >= 2147483648.0)
        return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(rounded);
}
}

void CPU::ExecuteFloatingPoint(const Instruction& ins) {
    if ((registers.MSR & CPURegisters::MSR_FP) == 0)
        throw CPUException(ExceptionKind::FloatingPointUnavailable,lastInstructionPC,
                           "Floating-point instruction while MSR[FP] is clear");
    const uint32_t raw=ins.raw, op=ins.opcode;
    const unsigned frt=FBits(raw,21), fra=FBits(raw,16), frb=FBits(raw,11), frc=FBits(raw,6);
    auto& f=registers.FPR;
    const bool rc=(raw&1u)!=0;
    auto recordFP=[&]() { if(rc) UpdateCR1(); };

    // Floating-point indexed and displacement loads/stores.
    if(op>=48 && op<=55) {
        const int32_t d=int16_t(raw&0xFFFFu);
        const uint32_t base=fra?registers.GPR[fra]:0u;
        const uint32_t ea=base+uint32_t(d);
        const bool single=(op==48||op==49||op==52||op==53);
        const bool store=(op>=52);
        const bool update=(op&1u)!=0;
        if(update && fra==0) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"Floating-point update form requires nonzero RA");
        const uint32_t alignment=single?4u:8u;
        if((ea&(alignment-1u))!=0) throw CPUException(ExceptionKind::Alignment,lastInstructionPC,"Unaligned floating-point memory access");
        try {
            if(store) {
                if(single) {
                    const float value = PairedSingleEnabled(registers) ? GetPS0(f[frt]) : static_cast<float>(f[frt]);
                    memory.Write32(ea,std::bit_cast<uint32_t>(value));
                }
                else memory.Write64(ea,std::bit_cast<uint64_t>(f[frt]));
            } else {
                if(single) {
                    const float value = std::bit_cast<float>(memory.Read32(ea));
                    f[frt] = PairedSingleEnabled(registers) ? PutPair(value, value) : static_cast<double>(value);
                }
                else f[frt]=std::bit_cast<double>(memory.Read64(ea));
            }
        } catch(const std::exception&) { throw CPUException(ExceptionKind::DataStorage,lastInstructionPC,"Floating-point memory access outside mapped memory"); }
        if(update) registers.GPR[fra]=ea;
        return;
    }

    if (op == 31) {
        const unsigned xo = (raw >> 1) & 0x3FFu;
        const bool load = xo == 535 || xo == 567 || xo == 599 || xo == 631;
        const bool single = xo == 535 || xo == 567 || xo == 663 || xo == 695;
        const bool update = xo == 567 || xo == 631 || xo == 695 || xo == 759;
        if (load || xo == 663 || xo == 695 || xo == 727 || xo == 759) {
            if (update && fra == 0)
                throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"Indexed floating-point update form requires nonzero RA");
            const uint32_t ea = (fra ? registers.GPR[fra] : 0u) + registers.GPR[frb];
            const unsigned alignment = single ? 4u : 8u;
            if ((ea & (alignment - 1u)) != 0)
                throw CPUException(ExceptionKind::Alignment,lastInstructionPC,"Unaligned indexed floating-point memory access");
            try {
                if (load) {
                    if (single) {
                        const float value = std::bit_cast<float>(memory.Read32(ea));
                        f[frt] = PairedSingleEnabled(registers) ? PutPair(value,value) : static_cast<double>(value);
                    } else f[frt] = std::bit_cast<double>(memory.Read64(ea));
                } else if (single) {
                    const float value = PairedSingleEnabled(registers) ? GetPS0(f[frt]) : static_cast<float>(f[frt]);
                    memory.Write32(ea,std::bit_cast<uint32_t>(value));
                } else memory.Write64(ea,std::bit_cast<uint64_t>(f[frt]));
            } catch (const std::exception&) {
                throw CPUException(ExceptionKind::DataStorage,lastInstructionPC,"Indexed floating-point memory access outside mapped memory");
            }
            if (update) registers.GPR[fra] = ea;
            return;
        }
    }

    if(op==63 && ((raw>>1)&0x3FFu)==0) { // fcmpu
        const unsigned bf=(raw>>23)&7u;
        uint32_t cr=2;
        if(std::isnan(f[fra])||std::isnan(f[frb])) cr=1;
        else if(f[fra]<f[frb]) cr=8;
        else if(f[fra]>f[frb]) cr=4;
        if(registers.GetXERSummaryOverflow()) cr|=1;
        registers.SetCRField(bf,cr); return;
    }
    if(op==63 && ((raw>>1)&0x3FFu)==32) { // fcmpo (ordered compare)
        const unsigned bf=(raw>>23)&7u;
        uint32_t cr=2;
        if(std::isnan(f[fra])||std::isnan(f[frb])) cr=1;
        else if(f[fra]<f[frb]) cr=8;
        else if(f[fra]>f[frb]) cr=4;
        registers.SetCRField(bf,cr); return;
    }

    if(op==59) { // single-precision A-form arithmetic
        const unsigned xo=(raw>>1)&31u;
        const bool paired = PairedSingleEnabled(registers);
        const double a=ToSingle(paired ? GetPS0(f[fra]) : f[fra]);
        const double b=ToSingle(paired ? GetPS0(f[frb]) : f[frb]);
        const double c=ToSingle(paired ? GetPS0(f[frc]) : f[frc]);
        float v=0.0f;
        switch(xo) {
        case 18: v=ToSingle(a/b); break;       // fdivs
        case 20: v=ToSingle(a-b); break;       // fsubs
        case 21: v=ToSingle(a+b); break;       // fadds
        case 22: v=ToSingle(std::sqrt(b)); break; // fsqrts
        case 24: v=ToSingle(1.0/b); break;      // fres estimate
        case 25: v=ToSingle(a*c); break;       // fmuls
        case 28: v=std::fma(float(a),float(c),-float(b)); break; // fmsubs
        case 29: v=std::fma(float(a),float(c),float(b)); break; // fmadds
        case 30: v=-std::fma(float(a),float(c),-float(b)); break;
        case 31: v=-std::fma(float(a),float(c),float(b)); break;
        default: throw CPUException(ExceptionKind::UnimplementedInstruction,lastInstructionPC,"Unsupported single-precision FP A-form opcode");
        }
        f[frt]=paired ? PutPair(v,v) : static_cast<double>(v); recordFP(); return;
    }

    if(op==63) {
        const unsigned xo5=(raw>>1)&31u;
        // A-form operations carry FRC in bits 6..10, so decode the five-bit XO.
        if(xo5==18 || xo5==20 || xo5==21 || xo5==22 || xo5==23 || xo5==25 ||
           xo5==28 || xo5==29 || xo5==30 || xo5==31) {
            const double aa=f[fra], bb=f[frb], cc=f[frc];
            double av=0;
            switch(xo5) {
            case 18: av=aa/bb; break; case 20: av=aa-bb; break;
            case 21: av=aa+bb; break; case 22: av=std::sqrt(bb); break;
            case 23: av=(aa>=0.0)?cc:bb; break; case 25: av=aa*cc; break;
            case 28: av=aa*cc-bb; break; case 29: av=aa*cc+bb; break;
            case 30: av=-(aa*cc-bb); break; case 31: av=-(aa*cc+bb); break;
            }
            f[frt]=av; recordFP(); return;
        }
        const unsigned xo=(raw>>1)&0x3FFu;
        double v=0;
        switch(xo) {
        case 38: { // mtfsb1; PowerPC bit numbers are MSB-first and FPSCR[1:2] are read-only summaries.
            const unsigned bit = frt;
            if (bit != 1 && bit != 2) registers.FPSCR |= 1u << (31u - bit);
            recordFP(); return;
        }
        case 12: v=ToSingle(f[frb]); break;                    // frsp
        case 14: v=std::bit_cast<double>(0xFFF8000000000000ull | uint32_t(ConvertToInteger(f[frb],false,registers.FPSCR))); break;
        case 15: v=std::bit_cast<double>(0xFFF8000000000000ull | uint32_t(ConvertToInteger(f[frb],true,registers.FPSCR))); break;
        case 40: v=-f[frb]; break;                             // fneg
        case 72: v=f[frb]; break;                              // fmr
        case 136: v=-std::fabs(f[frb]); break;                 // fnabs
        case 264: v=std::fabs(f[frb]); break;                  // fabs
        case 583: { // mffs: FPSCR copied to low word of FPR
            const uint64_t bits=0xFFF8000000000000ull | registers.FPSCR;
            f[frt]=std::bit_cast<double>(bits); return;
        }
        case 711: { // mtfsf (simplified field mask)
            const unsigned fm=(raw>>17)&0xFFu;
            const uint32_t value=uint32_t(std::bit_cast<uint64_t>(f[frb]));
            for(unsigned n=0;n<8;++n) if(fm&(1u<<(7-n))) {
                const uint32_t shift=28u-4u*n;
                registers.FPSCR=(registers.FPSCR&~(0xFu<<shift))|(value&(0xFu<<shift));
            }
            recordFP(); return;
        }
        case 814: v=std::sqrt(f[frb]); break;                  // fsqrt
        case 18: v=f[fra]/f[frb]; break;                       // fdiv
        case 20: v=f[fra]-f[frb]; break;                       // fsub
        case 21: v=f[fra]+f[frb]; break;                       // fadd
        case 22: v=std::sqrt(f[frb]); break;                   // fsqrt (alternate encoding)
        case 23: v=!std::signbit(f[fra])?f[frc]:f[frb]; break; // fsel
        case 25: v=f[fra]*f[frc]; break;                       // fmul
        case 28: v=f[fra]*f[frc]-f[frb]; break;               // fmsub
        case 29: v=f[fra]*f[frc]+f[frb]; break;               // fmadd
        case 30: v=-(f[fra]*f[frc]-f[frb]); break;            // fnmsub
        case 31: v=-(f[fra]*f[frc]+f[frb]); break;            // fnmadd
        default: throw CPUException(ExceptionKind::UnimplementedInstruction,lastInstructionPC,"Unsupported double-precision FP opcode");
        }
        f[frt]=v; recordFP(); return;
    }
    throw CPUException(ExceptionKind::UnimplementedInstruction,lastInstructionPC,"Unsupported floating-point instruction");
}
}
