#include "cpu.hpp"

namespace Broadway {
namespace {
bool BranchCondition(CPURegisters& r, uint32_t bo, uint32_t bi, bool allowCTR) {
    bool ctrOK=true, condOK=true;
    if (!(bo & 4u)) {
        if (allowCTR) --r.CTR;
        ctrOK = ((r.CTR != 0) != ((bo & 2u) != 0));
    }
    if (!(bo & 16u)) {
        const bool bit=((r.CR >> (31u-bi)) & 1u) != 0;
        condOK = bit == ((bo & 8u) != 0);
    }
    return ctrOK && condOK;
}
int32_t SignExtendBranch(uint32_t value, unsigned bits) {
    const uint32_t sign=1u<<(bits-1);
    const uint32_t mask=(1u<<bits)-1u;
    value &= mask;
    return int32_t((value ^ sign)-sign);
}
}
void CPU::ExecuteBranch(const Instruction& ins) {
    const uint32_t raw=ins.raw, op=ins.opcode;
    const uint32_t cia=lastInstructionPC;
    if(op==18) {
        const int32_t disp=SignExtendBranch(raw & 0x03FFFFFCu,26);
        if(raw&1u) registers.LR=registers.PC;
        registers.PC=(raw&2u)?uint32_t(disp):cia+uint32_t(disp);
        return;
    }
    if(op==16) {
        const uint32_t bo=(raw>>21)&31u, bi=(raw>>16)&31u;
        const int32_t disp=SignExtendBranch(raw&0xFFFCu,16);
        const bool take=BranchCondition(registers,bo,bi,true);
        if(raw&1u) registers.LR=registers.PC;
        if(take) registers.PC=(raw&2u)?uint32_t(disp):cia+uint32_t(disp);
        return;
    }
    if(op==19) {
        const uint32_t xo=(raw>>1)&0x3FFu;
        if (xo == 150) return; // Prior work is committed at each interpreter boundary.
        const uint32_t bo=(raw>>21)&31u, bi=(raw>>16)&31u;
        if(xo==0) { // mcrf
            const unsigned crfd=(raw>>23)&7u;
            const unsigned crfs=(raw>>18)&7u;
            registers.SetCRField(crfd,registers.GetCRField(crfs));
            return;
        }
        if(xo==50) {
            if (registers.MSR & CPURegisters::MSR_PR)
                throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged rfi executed in user mode");
            registers.MSR = registers.SRR1;
            // SRR0 contains the restart address. Exception entry normally
            // supplies an aligned address; preserve its exact value here so
            // a malformed/restored state is reported as an instruction-fetch
            // alignment fault instead of being silently rounded down.
            registers.PC = registers.SRR0;
            return;
        }
        if(xo==16) { // bclr
            const uint32_t target=registers.LR&~3u; // capture before LK update
            const bool take=BranchCondition(registers,bo,bi,true);
            if(raw&1u) registers.LR=registers.PC;
            if(take) registers.PC=target;
            return;
        }
        if(xo==528) { // bcctr: CTR supplies target and is not decremented
            if(!(bo & 4u))
                throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"bcctr cannot test/decrement CTR");
            const uint32_t target=registers.CTR&~3u;
            const bool take=BranchCondition(registers,bo,bi,false);
            if(raw&1u) registers.LR=registers.PC;
            if(take) registers.PC=target;
            return;
        }
        // PowerPC CR logical instructions share primary opcode 19 with branches.
        if(xo==33 || xo==129 || xo==193 || xo==225 || xo==257 || xo==289 || xo==417 || xo==449) {
            ExecuteInteger(ins);
            return;
        }
    }
    throw CPUException(ExceptionKind::UnimplementedInstruction,lastInstructionPC,"Unsupported branch instruction");
}
}
