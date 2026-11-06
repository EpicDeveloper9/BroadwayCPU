#pragma once
#include "registers.hpp"
#include "memory.hpp"
#include "decoder.hpp"
#include "exceptions.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace Broadway {
class CPU {
public:
    explicit CPU(size_t memorySize);
    void Reset();
    void Step();
    void Run(uint32_t instructionCount);
    void RunUntilHalt(uint64_t maxInstructions);

    CPURegisters registers;
    Memory memory;
    bool halted = false;
    bool traceEnabled = false;
    std::function<void(const std::string&)> traceCallback;

    uint64_t InstructionsExecuted() const { return instructionCounter; }
    uint32_t LastInstructionPC() const { return lastInstructionPC; }

private:
    uint64_t instructionCounter = 0;
    uint32_t lastInstructionPC = 0;
    void Execute(const Instruction&);
    void ExecuteInteger(const Instruction&);
    void ExecuteBranch(const Instruction&);
    void ExecuteLoadStore(const Instruction&);
    void ExecuteSystem(const Instruction&);
    void ExecuteFloatingPoint(const Instruction&);
    void ExecutePairedSingle(const Instruction&);
    void UpdateCR0(uint32_t result);
    void SetCRField(unsigned field, uint32_t result);
    void UpdateCR1();
    void SetOverflow(bool overflow);
};
}
