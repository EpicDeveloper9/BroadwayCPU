#include "cpu.hpp"
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Broadway {
namespace {
uint32_t FBits(uint32_t raw, unsigned shift) { return (raw >> shift) & 31u; }
float ToSingle(double x) { return static_cast<float>(x); }
}

void CPU::ExecuteFloatingPoint(const Instruction& ins) {
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
                if(single) memory.Write32(ea,std::bit_cast<uint32_t>(static_cast<float>(f[frt])));
                else memory.Write64(ea,std::bit_cast<uint64_t>(f[frt]));
            } else {
                if(single) f[frt]=static_cast<double>(std::bit_cast<float>(memory.Read32(ea)));
                else f[frt]=std::bit_cast<double>(memory.Read64(ea));
            }
        } catch(const std::exception&) { throw CPUException(ExceptionKind::DataStorage,lastInstructionPC,"Floating-point memory access outside mapped memory"); }
        if(update) registers.GPR[fra]=ea;
        return;
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
        const double a=ToSingle(f[fra]), b=ToSingle(f[frb]), c=ToSingle(f[frc]);
        double v=0;
        switch(xo) {
        case 18: v=ToSingle(a/b); break;       // fdivs
        case 20: v=ToSingle(a-b); break;       // fsubs
        case 21: v=ToSingle(a+b); break;       // fadds
        case 22: v=ToSingle(std::sqrt(b)); break; // fsqrts
        case 24: v=ToSingle(1.0/b); break;      // fres approximation
        case 25: v=ToSingle(a*c); break;       // fmuls
        case 28: v=ToSingle(a*c-b); break;     // fmsubs
        case 29: v=ToSingle(a*c+b); break;     // fmadds
        case 30: v=ToSingle(-(a*c-b)); break;  // fnmsubs
        case 31: v=ToSingle(-(a*c+b)); break;  // fnmadds
        default: throw CPUException(ExceptionKind::UnimplementedInstruction,lastInstructionPC,"Unsupported single-precision FP A-form opcode");
        }
        f[frt]=v; recordFP(); return;
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
        case 12: v=ToSingle(f[frb]); break;                    // frsp
        case 14: v=static_cast<double>(static_cast<int32_t>(f[frb])); break; // fctiw (simplified result representation)
        case 15: v=static_cast<double>(static_cast<int32_t>(std::trunc(f[frb]))); break; // fctiwz
        case 40: v=-f[frb]; break;                             // fneg
        case 72: v=f[frb]; break;                              // fmr
        case 136: v=-f[frb]; break;                            // fnabs
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
        case 23: v=(f[fra]>=0.0)?f[frc]:f[frb]; break;         // fsel
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
