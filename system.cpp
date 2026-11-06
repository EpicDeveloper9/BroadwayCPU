#include "cpu.hpp"
namespace Broadway {
void CPU::ExecuteSystem(const Instruction&) {
    throw CPUException(ExceptionKind::SystemCall,lastInstructionPC,
                       "System call instruction (sc) encountered; syscall interface is not connected yet");
}
}
