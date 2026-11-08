#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>

namespace Broadway {
enum class ExceptionKind : uint8_t {
    UnimplementedInstruction, InvalidInstruction, DataStorage, InstructionStorage,
    Alignment, Program, SystemCall, FloatingPointUnavailable
};

class CPUException : public std::runtime_error {
public:
    CPUException(ExceptionKind kind, uint32_t pc, const std::string& message);
    ExceptionKind Kind() const noexcept { return kind; }
    uint32_t FaultPC() const noexcept { return pc; }
private:
    ExceptionKind kind;
    uint32_t pc;
};
}
