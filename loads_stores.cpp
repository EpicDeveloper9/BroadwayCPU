#include "cpu.hpp"
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
    switch(op) {
    case 32: g[rt]=read32(address); break;
    case 33: if(!ra||ra==rt) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"Invalid lwzu operands"); g[rt]=read32(address); g[ra]=address; break;
    case 34: g[rt]=memory.Read8(address); break;
    case 35: if(!ra||ra==rt) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"Invalid lbzu operands"); g[rt]=memory.Read8(address); g[ra]=address; break;
    case 36: write32(address,g[rt]); break;
    case 37: if(!ra) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"Invalid stwu operands"); write32(address,g[rt]); g[ra]=address; break;
    case 38: memory.Write8(address,uint8_t(g[rt])); break;
    case 39: if(!ra) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"Invalid stbu operands"); memory.Write8(address,uint8_t(g[rt])); g[ra]=address; break;
    case 40: g[rt]=read16(address); break;
    case 41: if(!ra||ra==rt) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"Invalid lhzu operands"); g[rt]=read16(address); g[ra]=address; break;
    case 42: g[rt]=uint32_t(int32_t(int16_t(read16(address)))); break;
    case 43: if(!ra||ra==rt) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"Invalid lhau operands"); g[rt]=uint32_t(int32_t(int16_t(read16(address)))); g[ra]=address; break;
    case 44: write16(address,uint16_t(g[rt])); break;
    case 45: if(!ra) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"Invalid sthu operands"); write16(address,uint16_t(g[rt])); g[ra]=address; break;
    case 46:
        for(uint32_t r=rt;r<32;++r) g[r]=read32(address+(r-rt)*4u);
        break;
    case 47:
        for(uint32_t r=rt;r<32;++r) write32(address+(r-rt)*4u,g[r]);
        break;
    default: throw CPUException(ExceptionKind::UnimplementedInstruction,lastInstructionPC,"Unsupported load/store instruction");
    }
}
}
