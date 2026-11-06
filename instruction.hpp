#pragma once
#include <cstdint>

namespace Broadway {
enum class InstructionType : uint8_t {
    Unknown, Integer, Branch, LoadStore, Compare, System, FloatingPoint, PairedSingle
};

struct Instruction {
    uint32_t raw = 0;
    InstructionType type = InstructionType::Unknown;
    uint8_t opcode = 0;
    uint8_t rt = 0; // RT or RS, depending on instruction
    uint8_t ra = 0;
    uint8_t rb = 0;
    uint16_t extendedOpcode = 0;
    int32_t immediate = 0;
    bool record = false;
    bool link = false;
    bool absolute = false;
    bool oe = false;
    uint8_t rc = 0;
};
}
