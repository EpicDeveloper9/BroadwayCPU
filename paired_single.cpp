#include "cpu.hpp"
#include <bit>
#include <cmath>
#include <cstdint>

namespace Broadway {
namespace {
struct Pair { float a,b; };
Pair GetPair(double d) {
    const uint64_t bits=std::bit_cast<uint64_t>(d);
    return {std::bit_cast<float>(uint32_t(bits>>32)),std::bit_cast<float>(uint32_t(bits))};
}
double PutPair(float a,float b) {
    const uint64_t bits=(uint64_t(std::bit_cast<uint32_t>(a))<<32)|std::bit_cast<uint32_t>(b);
    return std::bit_cast<double>(bits);
}
}
void CPU::ExecutePairedSingle(const Instruction& ins) {
    const uint32_t raw=ins.raw;
    const unsigned frt=(raw>>21)&31u, fra=(raw>>16)&31u, frb=(raw>>11)&31u, frc=(raw>>6)&31u;
    const unsigned xo=(raw>>1)&0x3FFu;
    auto& f=registers.FPR;
    Pair a=GetPair(f[fra]), b=GetPair(f[frb]), c=GetPair(f[frc]);
    float x=0,y=0;
    switch(xo) {
    case 18: x=a.a/b.a; y=a.b/b.b; break;                 // ps_div
    case 20: x=a.a-b.a; y=a.b-b.b; break;                 // ps_sub
    case 21: x=a.a+b.a; y=a.b+b.b; break;                 // ps_add
    case 23: x=(a.a>=0)?c.a:b.a; y=(a.b>=0)?c.b:b.b; break; // ps_sel
    case 25: x=a.a*c.a; y=a.b*c.b; break;                 // ps_mul
    case 28: x=a.a*c.a-b.a; y=a.b*c.b-b.b; break;        // ps_msub
    case 29: x=a.a*c.a+b.a; y=a.b*c.b+b.b; break;        // ps_madd
    case 30: x=-(a.a*c.a-b.a); y=-(a.b*c.b-b.b); break;  // ps_nmsub
    case 31: x=-(a.a*c.a+b.a); y=-(a.b*c.b+b.b); break;  // ps_nmadd
    case 10: x=a.a+c.a; y=b.a; break;                     // ps_sum0
    case 11: x=b.a; y=a.b+c.b; break;                     // ps_sum1
    case 12: x=a.a*c.a; y=a.b; break;                     // ps_muls0
    case 13: x=a.a*c.b; y=a.b; break;                     // ps_muls1
    case 14: x=a.a*c.a+b.a; y=a.b; break;                 // ps_madds0
    case 15: x=a.a*c.b+b.a; y=a.b; break;                 // ps_madds1
    case 40: x=-a.a; y=-a.b; break;                       // ps_neg
    case 72: x=a.a; y=a.b; break;                         // ps_mr
    case 136: x=std::fabs(a.a); y=std::fabs(a.b); break;  // ps_abs
    case 264: x=-std::fabs(a.a); y=-std::fabs(a.b); break;// ps_nabs
    case 528: x=a.a; y=b.a; break;                        // ps_merge00
    case 560: x=a.a; y=b.b; break;                        // ps_merge01
    case 592: x=a.b; y=b.a; break;                        // ps_merge10
    case 624: x=a.b; y=b.b; break;                        // ps_merge11
    default: throw CPUException(ExceptionKind::UnimplementedInstruction,lastInstructionPC,"Unsupported paired-single instruction");
    }
    f[frt]=PutPair(x,y);
    if(raw&1u) UpdateCR1();
}
}
