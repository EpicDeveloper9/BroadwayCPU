#include "cpu.hpp"
#include <bit>
#include <cstdint>

namespace Broadway {
void CPU::ExecuteLoadStore(const Instruction& ins) {
    const uint32_t raw=ins.raw, op=ins.opcode, rt=ins.rt, ra=ins.ra;
    const int32_t disp=int16_t(raw&0xFFFFu);
    const uint32_t base=ra?registers.GPR[ra]:0u;
    const uint32_t address=base+uint32_t(disp);
    auto& g=registers.GPR;
    auto align=[&](uint32_t a,uint32_t n) {
        if((a&(n-1u))!=0) throw CPUException(ExceptionKind::Alignment,lastInstructionPC,"Unaligned data access");
    };
    auto read32=[&](uint32_t a) {
        align(a,4);
        try { return memory.Read32(a); }
        catch(const std::exception&) { throw CPUException(ExceptionKind::DataStorage,lastInstructionPC,"Word load outside mapped memory"); }
    };
    auto read16=[&](uint32_t a) {
        align(a,2);
        try { return memory.Read16(a); }
        catch(const std::exception&) { throw CPUException(ExceptionKind::DataStorage,lastInstructionPC,"Halfword load outside mapped memory"); }
    };
    auto write32=[&](uint32_t a,uint32_t v) {
        align(a,4);
        try { memory.Write32(a,v); }
        catch(const std::exception&) { throw CPUException(ExceptionKind::DataStorage,lastInstructionPC,"Word store outside mapped memory"); }
    };
    auto write16=[&](uint32_t a,uint16_t v) {
        align(a,2);
        try { memory.Write16(a,v); }
        catch(const std::exception&) { throw CPUException(ExceptionKind::DataStorage,lastInstructionPC,"Halfword store outside mapped memory"); }
    };
    auto read8=[&](uint32_t a) {
        try { return memory.Read8(a); }
        catch(const std::exception&) { throw CPUException(ExceptionKind::DataStorage,lastInstructionPC,"Byte load outside mapped memory"); }
    };
    auto write8=[&](uint32_t a,uint8_t v) {
        try { memory.Write8(a,v); }
        catch(const std::exception&) { throw CPUException(ExceptionKind::DataStorage,lastInstructionPC,"Byte store outside mapped memory"); }
    };
    if (op == 31) {
        const unsigned xo = (raw >> 1) & 0x3FFu;
        if (xo == 983) {
            if ((registers.MSR & CPURegisters::MSR_FP) == 0)
                throw CPUException(ExceptionKind::FloatingPointUnavailable,lastInstructionPC,
                                   "stfiwx while MSR[FP] is clear");
            const uint32_t ea = (ra ? g[ra] : 0u) + g[ins.rb];
            align(ea,4);
            write32(ea,uint32_t(std::bit_cast<uint64_t>(registers.FPR[rt])));
            return;
        }
        if (xo == 534 || xo == 662 || xo == 790 || xo == 918) {
            const uint32_t ea = (ra ? g[ra] : 0u) + g[ins.rb];
            if (xo == 534 || xo == 662) {
                const uint32_t value = xo == 534 ? read32(ea) : g[rt];
                const uint32_t reversed = ((value & 0x000000FFu) << 24) |
                    ((value & 0x0000FF00u) << 8) | ((value & 0x00FF0000u) >> 8) |
                    ((value & 0xFF000000u) >> 24);
                if (xo == 534) g[rt] = reversed;
                else write32(ea, reversed);
            } else {
                const uint16_t value = xo == 790 ? read16(ea) : uint16_t(g[rt]);
                const uint16_t reversed = uint16_t((value << 8) | (value >> 8));
                if (xo == 790) g[rt] = reversed;
                else write16(ea, reversed);
            }
            return;
        }
    }
    switch(op) {
    case 32: g[rt]=read32(address); break;
    case 33: if(!ra||ra==rt) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"Invalid lwzu operands"); g[rt]=read32(address); g[ra]=address; break;
    case 34: g[rt]=read8(address); break;
    case 35: if(!ra||ra==rt) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"Invalid lbzu operands"); g[rt]=read8(address); g[ra]=address; break;
    case 36: write32(address,g[rt]); break;
    case 37: if(!ra) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"Invalid stwu operands"); write32(address,g[rt]); g[ra]=address; break;
    case 38: write8(address,uint8_t(g[rt])); break;
    case 39: if(!ra) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"Invalid stbu operands"); write8(address,uint8_t(g[rt])); g[ra]=address; break;
    case 40: g[rt]=read16(address); break;
    case 41: if(!ra||ra==rt) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"Invalid lhzu operands"); g[rt]=read16(address); g[ra]=address; break;
    case 42: g[rt]=uint32_t(int32_t(int16_t(read16(address)))); break;
    case 43: if(!ra||ra==rt) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"Invalid lhau operands"); g[rt]=uint32_t(int32_t(int16_t(read16(address)))); g[ra]=address; break;
    case 44: write16(address,uint16_t(g[rt])); break;
    case 45: if(!ra) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"Invalid sthu operands"); write16(address,uint16_t(g[rt])); g[ra]=address; break;
    case 46:
        if (ra != 0 && ra >= rt) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"lmw destination range includes its base register");
        for(uint32_t r=rt;r<32;++r) g[r]=read32(address+(r-rt)*4u);
        break;
    case 47:
        for(uint32_t r=rt;r<32;++r) write32(address+(r-rt)*4u,g[r]);
        break;
    default: throw CPUException(ExceptionKind::UnimplementedInstruction,lastInstructionPC,"Unsupported load/store instruction");
    }
}
}
