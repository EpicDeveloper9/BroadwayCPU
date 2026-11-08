#include "cpu.hpp"
#include <iomanip>
#include <sstream>

namespace Broadway {
CPU::CPU(size_t memorySize) : memory(memorySize) { Reset(); }
void CPU::Reset() {
    registers.Reset(); halted = false; instructionCounter = 0; lastInstructionPC = 0; breakpointHit.reset();
}
void CPU::Step() {
    if (halted) return;
    lastInstructionPC = registers.PC;
    if ((registers.PC & 3u) != 0)
        throw CPUException(ExceptionKind::Alignment, registers.PC, "Unaligned instruction fetch");
    uint32_t raw;
    try { raw = memory.Read32(registers.PC); }
    catch (const std::exception&) {
        throw CPUException(ExceptionKind::InstructionStorage, registers.PC, "Instruction fetch outside mapped memory");
    }
    registers.PC += 4;
    const Instruction ins = Decoder::Decode(raw);
    if (traceEnabled && traceCallback) {
        std::ostringstream s;
        s << std::hex << std::setfill('0') << std::setw(8) << lastInstructionPC
          << "  " << std::setw(8) << raw << "  " << Decoder::Name(ins);
        traceCallback(s.str());
    }
    try {
        if (ins.type == InstructionType::Invalid)
            throw CPUException(ExceptionKind::InvalidInstruction, lastInstructionPC, "Invalid PowerPC instruction encoding");
        if (!ins.implemented)
            throw CPUException(ExceptionKind::UnimplementedInstruction, lastInstructionPC,
                               std::string("Unsupported instruction: ") + Decoder::Name(ins));
        Execute(ins);
    } catch (const CPUException&) {
        registers.PC = lastInstructionPC;
        throw;
    }
    ++instructionCounter;
}
void CPU::Run(uint32_t count) {
    breakpointHit.reset();
    for (uint32_t n=0; n<count && !halted; ++n) {
        if (breakpoints.contains(registers.PC)) { breakpointHit = registers.PC; return; }
        Step();
    }
}
void CPU::RunUntilHalt(uint64_t maxInstructions) {
    breakpointHit.reset();
    uint64_t n=0;
    while (!halted && n<maxInstructions) {
        if (breakpoints.contains(registers.PC)) { breakpointHit = registers.PC; return; }
        Step(); ++n;
    }
}
void CPU::Execute(const Instruction& i) {
    switch (i.type) {
    case InstructionType::Integer: case InstructionType::Compare: ExecuteInteger(i); break;
    case InstructionType::Branch: ExecuteBranch(i); break;
    case InstructionType::LoadStore: ExecuteLoadStore(i); break;
    case InstructionType::System: ExecuteSystem(i); break;
    case InstructionType::FloatingPoint: ExecuteFloatingPoint(i); break;
    case InstructionType::PairedSingle: ExecutePairedSingle(i); break;
    case InstructionType::Unsupported:
        throw CPUException(ExceptionKind::UnimplementedInstruction,lastInstructionPC,"Unsupported PowerPC instruction");
    case InstructionType::Invalid:
        throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"Invalid PowerPC instruction encoding");
    default:
        throw CPUException(ExceptionKind::UnimplementedInstruction,lastInstructionPC,"Unsupported PowerPC instruction");
    }
}
void CPU::SetCRField(unsigned field, uint32_t result) {
    uint32_t v = (int32_t(result)<0) ? 8u : (result>0 ? 4u : 2u);
    if (registers.GetXERSummaryOverflow()) v |= 1u;
    registers.SetCRField(field,v);
}
void CPU::UpdateCR0(uint32_t result) { SetCRField(0,result); }
void CPU::UpdateCR1() { registers.SetCRField(1, registers.FPSCR >> 28); }
void CPU::SetOverflow(bool overflow) { registers.SetXEROverflow(overflow); }
}
