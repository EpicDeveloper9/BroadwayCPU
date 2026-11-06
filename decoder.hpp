#pragma once
#include "instruction.hpp"
namespace Broadway {
class Decoder {
public:
    static Instruction Decode(uint32_t raw);
    static const char* Name(const Instruction& instruction);
};
}
