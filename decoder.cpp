#include "decoder.hpp"
namespace Broadway {
Instruction Decoder::Decode(uint32_t raw) {
    Instruction i{};
    i.raw = raw;
    i.opcode = uint8_t(raw >> 26);
    i.rt = uint8_t((raw >> 21) & 31);
    i.ra = uint8_t((raw >> 16) & 31);
    i.rb = uint8_t((raw >> 11) & 31);
    i.record = (raw & 1) != 0;
    i.rc = uint8_t(raw & 1);
    i.link = (raw & 1) != 0;
    i.absolute = (raw & 2) != 0;
    i.oe = (raw & 0x400) != 0;
    i.extendedOpcode = uint16_t((raw >> 1) & 0x3FF);
    i.immediate = int16_t(raw & 0xFFFF);

    switch (i.opcode) {
    case 4: i.type = InstructionType::PairedSingle; break;
    case 7: case 8: case 12: case 13: case 14: case 15:
    case 20: case 21: case 23: case 24: case 25: case 26: case 27: case 28: case 29:
    case 31: i.type = InstructionType::Integer; break;
    case 10: case 11: i.type = InstructionType::Compare; break;
    case 16: case 18: case 19: i.type = InstructionType::Branch; break;
    case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39:
    case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
        i.type = InstructionType::LoadStore; break;
    case 17: i.type = InstructionType::System; break;
    case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55:
    case 56: case 57: case 58: case 59: case 60: case 61: case 62: case 63:
        i.type = InstructionType::FloatingPoint; break;
    default: i.type = InstructionType::Unknown; break;
    }
    if (i.opcode == 18) {
        int32_t d = int32_t(raw & 0x03FFFFFC);
        if (d & 0x02000000) d |= int32_t(0xFC000000);
        i.immediate = d;
    } else if (i.opcode == 16) {
        i.immediate = int16_t(raw & 0xFFFC);
    }
    return i;
}
const char* Decoder::Name(const Instruction& i) {
    switch (i.opcode) {
    case 7: return "mulli"; case 8: return "subfic";
    case 10: return "cmpli"; case 11: return "cmpi";
    case 12: return "addic"; case 13: return "addic.";
    case 14: return "addi"; case 15: return "addis";
    case 16: return "bc"; case 18: return "b"; case 19: return "branch-XO";
    case 20: return "rlwimi"; case 21: return "rlwinm"; case 23: return "rlwnm";
    case 24: return "ori"; case 25: return "oris"; case 26: return "xori";
    case 27: return "xoris"; case 28: return "andi."; case 29: return "andis.";
    case 4: return "paired-single";
    case 31: return "integer-XO / indexed-memory";
    case 32: return "lwz"; case 33: return "lwzu"; case 34: return "lbz";
    case 35: return "lbzu"; case 36: return "stw"; case 37: return "stwu";
    case 38: return "stb"; case 39: return "stbu"; case 40: return "lhz";
    case 41: return "lhzu"; case 42: return "lha"; case 43: return "lhau";
    case 44: return "sth"; case 45: return "sthu"; case 46: return "lmw";
    case 47: return "stmw";
    case 48: return "lfs"; case 49: return "lfsu"; case 50: return "lfd"; case 51: return "lfdu";
    case 52: return "stfs"; case 53: return "stfsu"; case 54: return "stfd"; case 55: return "stfdu";
    default: return "unknown";
    }
}
}
