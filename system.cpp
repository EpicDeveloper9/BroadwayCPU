#include "cpu.hpp"
namespace Broadway {
void CPU::EnterException(uint32_t vector, uint32_t savedPC) {
    const uint32_t oldMsr = registers.MSR;
    registers.SRR0 = savedPC;
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
    registers.PC = prefix | vector;
}

void CPU::ExecuteSystem(const Instruction&) {
    // sc saves the following instruction so rfi resumes after the syscall.
    // The guest OS remains responsible for installing and servicing the vector.
    EnterException(0x00000C00u, registers.PC);
}
}
