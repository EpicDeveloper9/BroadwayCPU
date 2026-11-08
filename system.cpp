#include "cpu.hpp"
namespace Broadway {
void CPU::ExecuteSystem(const Instruction&) {
    // sc is an architectural program exception.  The saved PC is the
    // instruction after sc; returning from the vector with rfi therefore
    // resumes at the following instruction.  The boot ROM/OS normally
    // installs the handler at 0x80000c00 (or the high vector when MSR[IP]
    // is set).  Keep service dispatch out of the CPU core.
    const uint32_t oldMsr = registers.MSR;
    registers.SRR0 = registers.PC;
    registers.SRR1 = oldMsr;
    constexpr uint32_t exceptionClearedBits =
        (1u << 18) | // POW
        (1u << 15) | // EE
        (1u << 17) | // PR
        (1u << 13) | // FP
        (1u << 11) | // FE0
        (1u << 10) | // SE
        (1u << 9)  | // BE
        (1u << 8)  | // FE1
        (1u << 5)  | // IR
        (1u << 4)  | // DR
        (1u << 1);   // RI
    const uint32_t prefix = (oldMsr & (1u << 6)) ? 0xFFF00000u : 0u;
    registers.MSR = oldMsr & ~exceptionClearedBits;
    registers.PC = prefix | 0x00000C00u;
}
}
