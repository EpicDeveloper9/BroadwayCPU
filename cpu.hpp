#pragma once
#include "registers.hpp"
#include "memory.hpp"
#include "decoder.hpp"
#include "exceptions.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>

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
    void AddBreakpoint(uint32_t address) { breakpoints.insert(address); }
    bool RemoveBreakpoint(uint32_t address) { return breakpoints.erase(address) != 0; }
    void ClearBreakpoints() { breakpoints.clear(); breakpointHit.reset(); }
    bool HasBreakpoint(uint32_t address) const { return breakpoints.contains(address); }
    const std::unordered_set<uint32_t>& Breakpoints() const { return breakpoints; }
    std::optional<uint32_t> BreakpointHit() const { return breakpointHit; }

private:
    uint64_t instructionCounter = 0;
    uint32_t lastInstructionPC = 0;
    std::unordered_set<uint32_t> breakpoints;
    std::optional<uint32_t> breakpointHit;
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
