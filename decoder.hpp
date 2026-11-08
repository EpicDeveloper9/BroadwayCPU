#pragma once
#include "instruction.hpp"
#include <string>
namespace Broadway {
class Decoder {
public:
    static Instruction Decode(uint32_t raw);
    static const char* Name(const Instruction& instruction);
    static std::string Format(const Instruction& instruction, uint32_t address);
};
}
