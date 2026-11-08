#include "decoder.hpp"

#include <array>
#include <iomanip>
#include <sstream>

namespace Broadway {
namespace {
struct XOName { uint16_t xo; const char* name; };

constexpr XOName kIntegerXO[]{
    XOName{8,"subfc"}, {10,"addc"}, {11,"mulhwu"}, {19,"mfcr"},
    {23,"lwzx"}, {24,"slw"}, {26,"cntlzw"}, {28,"and"}, {32,"cmplw"},
    {40,"subf"}, {54,"dcbst"}, {55,"lwzux"}, {60,"andc"}, {75,"mulhw"},
    {83,"mfmsr"}, {86,"dcbf"}, {87,"lbzx"}, {104,"neg"}, {119,"lbzux"},
    {124,"nor"}, {136,"subfe"}, {138,"adde"}, {144,"mtcrf"}, {146,"mtmsr"}, {150,"stwcx."},
    {151,"stwx"}, {178,"mtmsrd"}, {183,"stwux"}, {200,"subfze"},
    {202,"addze"}, {215,"stbx"}, {232,"subfme"}, {234,"addme"},
    {235,"mullw"}, {246,"dcbtst"}, {247,"stbux"}, {266,"add"}, {278,"dcbt"}, {279,"lhzx"},
    {284,"eqv"}, {316,"xor"}, {311,"lhzux"}, {339,"mfspr"},
    {343,"lhax"}, {371,"mftb"}, {375,"lhaux"}, {407,"sthx"}, {412,"orc"},
    {439,"sthux"}, {444,"or"}, {459,"divwu"}, {467,"mtspr"},
    {210,"mtsr"}, {242,"mtsrin"}, {476,"nand"}, {491,"divw"}, {533,"lswx"}, {534,"lwbrx"},
    {535,"lfsx"}, {536,"srw"}, {567,"lfsux"}, {595,"mfsr"}, {597,"lswi"}, {598,"sync"},
    {599,"lfdx"}, {631,"lfdux"}, {659,"mfsrin"}, {661,"stswx"}, {662,"stwbrx"},
    {663,"stfsx"}, {695,"stfsux"}, {725,"stswi"}, {727,"stfdx"},
    {759,"stfdux"}, {790,"lhbrx"}, {792,"sraw"}, {824,"srawi"},
    {854,"eieio"}, {918,"sthbrx"}, {922,"extsh"}, {982,"icbi"},
    {954,"extsb"}, {470,"dcbi"}, {1014,"dcbz"}
};

constexpr XOName kCrXO[]{
    XOName{0,"mcrf"}, {16,"bclr"}, {33,"crnor"}, {50,"rfi"},
    {64,"crandc"}, {129,"crandc"}, {150,"isync"}, {160,"crnand"},
    {193,"crxor"}, {224,"crnor"}, {257,"crand"}, {289,"creqv"},
    {417,"crorc"}, {449,"cror"}, {528,"bcctr"}, {577,"crandc"},
    {129,"crandc"}, {225,"crnand"}
};

const char* Find(const auto& table, uint16_t xo) {
    for (const auto& item : table) if (item.xo == xo) return item.name;
    return nullptr;
}

bool HasOEForm(uint32_t xo);
uint32_t XOpcode(uint32_t raw);

const char* PrimaryName(const Instruction& i) {
    switch (i.opcode) {
    case 4: {
        const unsigned xo5 = (i.raw >> 1) & 31u;
        switch (xo5) {
        case 10: return "ps_sum0"; case 11: return "ps_sum1";
        case 12: return "ps_muls0"; case 13: return "ps_muls1";
        case 14: return "ps_madds0"; case 15: return "ps_madds1";
        case 18: return "ps_div"; case 20: return "ps_sub"; case 21: return "ps_add";
        case 23: return "ps_sel"; case 25: return "ps_mul"; case 28: return "ps_msub";
        case 29: return "ps_madd"; case 30: return "ps_nmsub"; case 31: return "ps_nmadd";
        default: break;
        }
        switch (i.extendedOpcode) {
        case 10: return "ps_sum0"; case 11: return "ps_sum1";
        case 12: return "ps_muls0"; case 13: return "ps_muls1";
        case 14: return "ps_madds0"; case 15: return "ps_madds1";
        case 18: return "ps_div"; case 20: return "ps_sub";
        case 21: return "ps_add"; case 23: return "ps_sel";
        case 25: return "ps_mul"; case 28: return "ps_msub";
        case 29: return "ps_madd"; case 30: return "ps_nmsub";
        case 31: return "ps_nmadd"; case 40: return "ps_neg";
        case 72: return "ps_mr"; case 136: return "ps_abs";
        case 264: return "ps_nabs"; case 528: return "ps_merge00";
        case 560: return "ps_merge01"; case 592: return "ps_merge10";
        case 624: return "ps_merge11"; case 0: return "ps_cmpu0";
        case 32: return "ps_cmpo0"; case 64: return "ps_cmpu1";
        case 96: return "ps_cmpo1";
        default: return nullptr;
        }
    }
    case 7: return "mulli"; case 8: return "subfic";
    case 10: return "cmplwi"; case 11: return "cmpwi";
    case 12: return "addic"; case 13: return "addic.";
    case 14: return "addi"; case 15: return "addis";
    case 16: return "bc"; case 18: return "b";
    case 19: {
        const uint16_t xo = i.extendedOpcode;
        if (xo == 0 || xo == 16 || xo == 528 || xo == 50 || xo == 150 ||
            xo == 33 || xo == 129 || xo == 193 || xo == 225 || xo == 257 ||
            xo == 289 || xo == 417 || xo == 449) return Find(kCrXO, xo);
        return nullptr;
    }
    case 20: return "rlwimi"; case 21: return "rlwinm"; case 23: return "rlwnm";
    case 24: return "ori"; case 25: return "oris"; case 26: return "xori";
    case 27: return "xoris"; case 28: return "andi."; case 29: return "andis.";
    case 31: {
        const uint16_t xo = uint16_t(XOpcode(i.raw));
        const char* name = Find(kIntegerXO, xo);
        if (name) return name;
        if (xo == 33) return "crnor";
        if (xo == 129) return "crandc";
        if (xo == 193) return "crxor";
        if (xo == 225) return "crnand";
        if (xo == 257) return "crand";
        if (xo == 289) return "creqv";
        if (xo == 417) return "crorc";
        if (xo == 449) return "cror";
        if (xo == 0) return "cmpw";
        if (xo == 32) return "cmplw";
        return nullptr;
    }
    case 17: return (i.raw & 0x03FFFFFCu) == 0 ? "sc" : nullptr;
    case 32: return "lwz"; case 33: return "lwzu";
    case 34: return "lbz"; case 35: return "lbzu";
    case 36: return "stw"; case 37: return "stwu";
    case 38: return "stb"; case 39: return "stbu";
    case 40: return "lhz"; case 41: return "lhzu";
    case 42: return "lha"; case 43: return "lhau";
    case 44: return "sth"; case 45: return "sthu";
    case 46: return "lmw"; case 47: return "stmw";
    case 48: return "lfs"; case 49: return "lfsu";
    case 50: return "lfd"; case 51: return "lfdu";
    case 52: return "stfs"; case 53: return "stfsu";
    case 54: return "stfd"; case 55: return "stfdu";
    case 56: return "psq_l"; case 57: return "psq_lu";
    case 60: return "psq_st"; case 61: return "psq_stu";
    case 59: {
        switch ((i.raw >> 1) & 31u) {
        case 18: return "fdivs"; case 20: return "fsubs";
        case 21: return "fadds"; case 22: return "fsqrts";
        case 24: return "fres"; case 25: return "fmuls";
        case 28: return "fmsubs"; case 29: return "fmadds";
        case 30: return "fnmsubs"; case 31: return "fnmadds";
        default: return nullptr;
        }
    }
    case 63: {
        const unsigned xo5 = (i.raw >> 1) & 31u;
        switch (xo5) {
        case 18: return "fdiv"; case 20: return "fsub"; case 21: return "fadd";
        case 22: return "fsqrt"; case 23: return "fsel"; case 25: return "fmul";
        case 28: return "fmsub"; case 29: return "fmadd";
        case 30: return "fnmsub"; case 31: return "fnmadd";
        default: break;
        }
        const uint16_t xo10 = i.extendedOpcode;
        switch (xo10) {
        case 0: return "fcmpu"; case 32: return "fcmpo";
        case 12: return "frsp"; case 14: return "fctiw"; case 15: return "fctiwz";
        case 40: return "fneg"; case 72: return "fmr"; case 136: return "fnabs";
        case 264: return "fabs"; case 583: return "mffs"; case 711: return "mtfsf";
        case 38: return "mtfsb1";
        case 814: return "fsqrt";
        default: break;
        }
        return nullptr;
    }
    default: return nullptr;
    }
}

bool IsReservedPrimary(uint8_t op) {
    switch (op) {
    case 0: case 1: case 5: case 6: case 9: case 22: case 30:
    case 34 + 32: // 66 is reserved; 34 itself is a valid byte load.
        return true;
    default: return false;
    }
}

std::string Gpr(unsigned n) { return "r" + std::to_string(n); }
std::string Fpr(unsigned n) { return "f" + std::to_string(n); }
std::string Crf(unsigned n) { return "cr" + std::to_string(n); }
std::string Imm(int32_t value) { return std::to_string(value); }
std::string SPRName(unsigned spr) {
    switch (spr) {
    case 1: return "xer"; case 8: return "lr"; case 9: return "ctr";
    case 18: return "dsisr"; case 19: return "dar"; case 22: return "dec";
    case 25: return "sdr1"; case 26: return "srr0"; case 27: return "srr1";
    case 268: case 284: return "tbl"; case 269: case 285: return "tbu"; case 920: return "hid2";
    case 921: return "wpar"; case 952: return "mmcr0"; case 956: return "mmcr1"; case 1008: return "hid0"; case 1009: return "hid1";
    case 953: return "pmc1"; case 954: return "pmc2"; case 957: return "pmc3"; case 958: return "pmc4";
    case 1011: return "hid4";
    case 1017: return "l2cr";
    default: break;
    }
    if (spr >= 272 && spr <= 275) return "sprg" + std::to_string(spr - 272);
    if (spr >= 528 && spr <= 535) return "ibat" + std::to_string((spr - 528) / 2) + ((spr & 1u) ? "l" : "u");
    if (spr >= 536 && spr <= 543) return "dbat" + std::to_string((spr - 536) / 2) + ((spr & 1u) ? "l" : "u");
    if (spr >= 912 && spr <= 919) return "gqr" + std::to_string(spr - 912);
    return std::to_string(spr);
}

bool HasOEForm(uint32_t xo) {
    switch (xo) {
    case 8: case 10: case 40: case 104: case 136: case 138: case 200: case 202:
    case 232: case 234: case 235: case 266: case 459: case 491: return true;
    default: return false;
    }
}

uint32_t XOpcode(uint32_t raw) {
    const uint32_t full = (raw >> 1) & 0x3FFu;
    const uint32_t low = full & 0x1FFu;
    return HasOEForm(low) ? low : full;
}

bool IsImplemented(const Instruction& i) {
    const unsigned xo9 = XOpcode(i.raw);
    switch (i.opcode) {
    case 4:
        switch ((i.raw >> 1) & 31u) {
        case 10: case 11: case 12: case 13: case 14: case 15:
        case 18: case 20: case 21: case 23: case 25: case 28: case 29: case 30: case 31: return true;
        default: break;
        }
        switch (i.extendedOpcode) {
        case 0: case 32: case 64: case 96: case 10: case 11: case 12: case 13:
        case 14: case 15: case 18: case 20: case 21: case 23: case 25: case 28:
        case 29: case 30: case 31: case 40: case 72: case 136: case 264:
        case 528: case 560: case 592: case 624: return true;
        default: return false;
        }
    case 7: case 8: case 10: case 11: case 12: case 13: case 14: case 15:
    case 16: case 18: case 20: case 21: case 23: case 24: case 25: case 26:
    case 27: case 28: case 29: case 32: case 33: case 34: case 35: case 36:
    case 37: case 38: case 39: case 40: case 41: case 42: case 43: case 44:
    case 45: case 46: case 47: case 48: case 49: case 50: case 51: case 52:
    case 53: case 54: case 55: case 56: case 57: case 60: case 61: case 17:
        return true;
    case 19:
        return i.extendedOpcode == 0 || i.extendedOpcode == 16 || i.extendedOpcode == 50 || i.extendedOpcode == 150 || i.extendedOpcode == 528 ||
            i.extendedOpcode == 33 || i.extendedOpcode == 129 || i.extendedOpcode == 193 ||
            i.extendedOpcode == 225 || i.extendedOpcode == 257 || i.extendedOpcode == 289 ||
            i.extendedOpcode == 417 || i.extendedOpcode == 449;
    case 31:
        switch (xo9) {
        case 0: case 8: case 10: case 11: case 19: case 23: case 24: case 26: case 83: case 146:
        case 28: case 32: case 40: case 55: case 60: case 75: case 87: case 104:
        case 119: case 124: case 136: case 138: case 144: case 151: case 183:
        case 200: case 202: case 215: case 232: case 234: case 235: case 247:
        case 266: case 279: case 284: case 311: case 316: case 339: case 343:
        case 375: case 407: case 412: case 439: case 444: case 459: case 467:
        case 476: case 491: case 533: case 536: case 597: case 598: case 661:
        case 725: case 792: case 824: case 854: case 922: case 954:
        case 33: case 129: case 193: case 225: case 257: case 289: case 417: case 449:
        case 54: case 86: case 246: case 278: case 470: case 982: case 1014:
        case 210: case 242: case 371: case 534: case 535: case 567: case 595: case 599: case 631: case 659: case 662: case 663:
        case 695: case 727: case 759: case 790: case 918:
            return true;
        default: return false;
        }
    case 59: {
        const unsigned xo5 = (i.raw >> 1) & 31u;
        return xo5 == 18 || xo5 == 20 || xo5 == 21 || xo5 == 22 || xo5 == 24 ||
            xo5 == 25 || xo5 == 28 || xo5 == 29 || xo5 == 30 || xo5 == 31;
    }
    case 63: {
        const unsigned xo5 = (i.raw >> 1) & 31u;
        if (xo5 == 18 || xo5 == 20 || xo5 == 21 || xo5 == 22 || xo5 == 23 ||
            xo5 == 25 || xo5 == 28 || xo5 == 29 || xo5 == 30 || xo5 == 31) return true;
        return i.extendedOpcode == 0 || i.extendedOpcode == 32 || i.extendedOpcode == 12 ||
            i.extendedOpcode == 14 || i.extendedOpcode == 15 || i.extendedOpcode == 40 ||
            i.extendedOpcode == 72 || i.extendedOpcode == 136 || i.extendedOpcode == 264 ||
            i.extendedOpcode == 38 || i.extendedOpcode == 583 || i.extendedOpcode == 711 || i.extendedOpcode == 814;
    }
    default: return false;
    }
}
}

Instruction Decoder::Decode(uint32_t raw) {
    Instruction i{};
    i.raw = raw;
    i.opcode = uint8_t(raw >> 26);
    i.rt = uint8_t((raw >> 21) & 31u);
    i.ra = uint8_t((raw >> 16) & 31u);
    i.rb = uint8_t((raw >> 11) & 31u);
    i.record = (raw & 1u) != 0;
    i.rc = uint8_t(raw & 1u);
    i.link = (raw & 1u) != 0;
    i.absolute = (raw & 2u) != 0;
    i.oe = (raw & 0x400u) != 0 && HasOEForm((raw >> 1) & 0x1FFu);
    i.extendedOpcode = uint16_t((raw >> 1) & 0x3FFu);
    i.immediate = int16_t(raw & 0xFFFFu);

    switch (i.opcode) {
    case 4: case 56: case 57: case 60: case 61: i.type = InstructionType::PairedSingle; break;
    case 7: case 8: case 12: case 13: case 14: case 15:
    case 20: case 21: case 23: case 24: case 25: case 26: case 27: case 28: case 29:
        i.type = InstructionType::Integer; break;
    case 31: i.type = InstructionType::Integer; break;
    case 10: case 11: i.type = InstructionType::Compare; break;
    case 16: case 18: case 19: i.type = InstructionType::Branch; break;
    case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39:
    case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
        i.type = InstructionType::LoadStore; break;
    case 17: i.type = InstructionType::System; break;
    case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55:
    case 59: case 63: i.type = InstructionType::FloatingPoint; break;
    default: i.type = IsReservedPrimary(i.opcode) ? InstructionType::Invalid : InstructionType::Unsupported; break;
    }
    if (i.opcode == 31) {
        const unsigned xo = XOpcode(raw);
        if (xo == 535 || xo == 567 || xo == 599 || xo == 631 ||
            xo == 663 || xo == 695 || xo == 727 || xo == 759)
            i.type = InstructionType::FloatingPoint;
        if (xo == 534 || xo == 662 || xo == 790 || xo == 918)
            i.type = InstructionType::LoadStore;
    }

    if (i.opcode == 18) {
        int32_t d = int32_t(raw & 0x03FFFFFCu);
        if ((d & 0x02000000) != 0) d |= int32_t(0xFC000000u);
        i.immediate = d;
    } else if (i.opcode == 16) {
        i.immediate = int16_t(raw & 0xFFFCu);
    }

    i.recognized = PrimaryName(i) != nullptr;
    i.validityKnown = i.recognized;
    i.validEncoding = i.recognized;
    i.implemented = i.recognized && IsImplemented(i);
    if (i.opcode == 19 && i.extendedOpcode == 50 && raw != ((19u << 26) | (50u << 1))) {
        i.recognized = false;
        i.validityKnown = true;
        i.validEncoding = false;
        i.implemented = false;
        i.type = InstructionType::Invalid;
    }
    if (i.opcode == 31 && (i.extendedOpcode == 83 || i.extendedOpcode == 146) &&
        ((raw & 0x001FF800u) != 0 || (raw & 1u) != 0)) {
        i.recognized = false;
        i.validityKnown = true;
        i.validEncoding = false;
        i.implemented = false;
        i.type = InstructionType::Invalid;
    }
    if (i.opcode == 31 && (i.extendedOpcode == 210 || i.extendedOpcode == 242 ||
        i.extendedOpcode == 595 || i.extendedOpcode == 659) &&
        ((i.extendedOpcode == 210 || i.extendedOpcode == 595) ? (raw & (1u | (1u << 20))) != 0 : (raw & 1u) != 0)) {
        i.recognized = false;
        i.validityKnown = true;
        i.validEncoding = false;
        i.implemented = false;
        i.type = InstructionType::Invalid;
    }
    if (i.opcode == 31 && (i.extendedOpcode == 534 || i.extendedOpcode == 535 ||
        i.extendedOpcode == 567 || i.extendedOpcode == 599 || i.extendedOpcode == 631 ||
        i.extendedOpcode == 662 || i.extendedOpcode == 663 || i.extendedOpcode == 695 ||
        i.extendedOpcode == 727 || i.extendedOpcode == 759 || i.extendedOpcode == 790 ||
        i.extendedOpcode == 918) && (raw & 1u)) {
        i.recognized = false;
        i.implemented = false;
        i.type = InstructionType::Invalid;
    }
    if (i.opcode == 63 && i.extendedOpcode == 38 && (raw & 0x001FF800u) != 0) {
        i.recognized = false;
        i.validityKnown = true;
        i.validEncoding = false;
        i.implemented = false;
        i.type = InstructionType::Invalid;
    }
    if (!i.implemented && i.type != InstructionType::Invalid)
        i.type = InstructionType::Unsupported;
    if (i.opcode == 0 || IsReservedPrimary(i.opcode)) {
        i.type = InstructionType::Invalid;
        i.validityKnown = true;
        i.validEncoding = false;
    }
    // CPU::Step gates execution on `implemented` before calling Execute().
    // Therefore only encodings accepted by that gate have an execution path.
    i.dispatchable = i.implemented && i.type != InstructionType::Invalid;
    return i;
}

const char* Decoder::Name(const Instruction& i) {
    if (i.type == InstructionType::Invalid) return "illegal";
    if (const char* name = PrimaryName(i)) {
        if (i.opcode == 31 && i.oe) {
            thread_local std::string oeName;
            oeName = name;
            if (!oeName.empty() && oeName.back() == '.') oeName.pop_back();
            oeName += "o";
            if (i.record) oeName += ".";
            return oeName.c_str();
        }
        return name;
    }
    return i.type == InstructionType::Invalid ? "illegal" : "unsupported";
}

std::string Decoder::Format(const Instruction& i, uint32_t address) {
    const uint32_t raw = i.raw;
    const unsigned d = unsigned((raw >> 21) & 31u);
    const unsigned a = unsigned((raw >> 16) & 31u);
    const unsigned b = unsigned((raw >> 11) & 31u);
    const unsigned c = unsigned((raw >> 6) & 31u);
    const int32_t simm = int16_t(raw & 0xFFFFu);
    const uint32_t uimm = raw & 0xFFFFu;
    const char* mnemonic = Name(i);
    std::ostringstream out;
    out << mnemonic;

    if (!i.recognized) {
        out << " 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << raw;
        return out.str();
    }

    auto operands = [&] { out << "  "; };
    switch (i.opcode) {
    case 7: case 8: case 12: case 13: case 14: case 15:
        operands(); out << Gpr(d) << "," << Gpr(a) << "," << Imm(simm); break;
    case 10: case 11:
        operands(); out << Crf((raw >> 23) & 7u) << "," << Gpr(a) << "," << (i.opcode == 10 ? std::to_string(uimm) : Imm(simm)); break;
    case 16: {
        const uint32_t bo = (raw >> 21) & 31u, bi = (raw >> 16) & 31u;
        const int32_t disp = int16_t(raw & 0xFFFCu);
        const uint32_t target = (raw & 2u) ? uint32_t(disp) : address + uint32_t(disp);
        operands(); out << bo << "," << bi << ",0x" << std::hex << std::uppercase << target;
        if (raw & 1u) out << " (link)";
        break;
    }
    case 18: {
        const int32_t disp = int32_t(raw & 0x03FFFFFCu);
        const int32_t signDisp = (disp & 0x02000000) ? (disp | int32_t(0xFC000000u)) : disp;
        const uint32_t target = (raw & 2u) ? uint32_t(signDisp) : address + uint32_t(signDisp);
        operands(); out << "0x" << std::hex << std::uppercase << target;
        if (raw & 1u) out << " (link)";
        break;
    }
    case 19: {
        const unsigned xo = (raw >> 1) & 0x3FFu;
        if (xo == 0) { operands(); out << Crf((raw >> 23) & 7u) << "," << Crf((raw >> 18) & 7u); }
        else if (xo == 16 || xo == 528) { operands(); out << ((raw >> 21) & 31u) << "," << ((raw >> 16) & 31u); }
        else if (xo == 50 || xo == 150) { }
        else { operands(); out << "cr" << ((raw >> 21) & 31u) << ",cr" << ((raw >> 16) & 31u) << ",cr" << ((raw >> 11) & 31u); }
        break;
    }
    case 20: case 21: case 23: {
        operands(); out << Gpr(a) << "," << Gpr(d) << ",";
        if (i.opcode == 23) out << Gpr(b); else out << ((raw >> 11) & 31u);
        out << "," << ((raw >> 6) & 31u) << "," << ((raw >> 1) & 31u); break;
    }
    case 24: case 25: case 26: case 27: case 28: case 29:
        operands(); out << Gpr(a) << "," << Gpr(d) << "," << uimm; break;
    case 31: {
        const unsigned xo = XOpcode(raw);
        if (xo == 0 || xo == 32) { operands(); out << Crf((raw >> 23) & 7u) << "," << Gpr(a) << "," << Gpr(b); }
        else if (xo == 210) { operands(); out << "sr" << (a & 15u) << "," << Gpr(d); }
        else if (xo == 595) { operands(); out << Gpr(d) << ",sr" << (a & 15u); }
        else if (xo == 242 || xo == 659) { operands(); out << Gpr(d) << "," << Gpr(b); }
        else if (xo == 535 || xo == 567 || xo == 599 || xo == 631 || xo == 663 || xo == 695 || xo == 727 || xo == 759) {
            operands(); out << Fpr(d) << "," << Gpr(a) << "," << Gpr(b);
        }
        else if (xo == 19 || xo == 83 || xo == 339 || xo == 371) { operands(); const unsigned reg=((raw >> 16 & 31u) | ((raw >> 6 & 0x3E0u))); out << Gpr(d) << "," << SPRName(reg); }
        else if (xo == 144) { operands(); out << "0x" << std::hex << ((raw >> 12) & 0xFFu) << "," << Gpr(d); }
        else if (xo == 467 || xo == 146 || xo == 178) { operands(); out << SPRName((raw >> 16 & 31u) | ((raw >> 6 & 0x3E0u))) << "," << Gpr(d); }
        else if (xo == 533 || xo == 597 || xo == 661 || xo == 725) { operands(); out << Gpr(d) << "," << Gpr(a); if (xo == 533 || xo == 661) out << "," << Gpr(b); else out << "," << b; }
        else if (xo == 598 || xo == 854 || xo == 150 || xo == 86 || xo == 54 || xo == 278 || xo == 246 || xo == 982 || xo == 1014) { if (xo != 598 && xo != 854 && xo != 150) { operands(); out << Gpr(a) << "," << Gpr(b); } }
        else if (xo == 24 || xo == 536 || xo == 792 || xo == 824) { operands(); out << Gpr(d) << "," << Gpr(a) << ","; if (xo == 824) out << b; else out << Gpr(b); }
        else if (xo == 922 || xo == 954 || xo == 104 || xo == 26) { operands(); out << Gpr(d) << "," << Gpr(a); }
        else if (xo == 23 || xo == 55 || xo == 87 || xo == 119 || xo == 279 || xo == 311 || xo == 343 || xo == 375 || xo == 151 || xo == 183 || xo == 215 || xo == 247 || xo == 407 || xo == 439 || xo == 534 || xo == 662 || xo == 790 || xo == 918) {
            operands(); out << Gpr(d) << "," << Gpr(a) << "," << Gpr(b);
        } else if (xo == 235 || xo == 266 || xo == 8 || xo == 10 || xo == 40 || xo == 75 || xo == 11 || xo == 136 || xo == 138 || xo == 200 || xo == 202 || xo == 232 || xo == 234 || xo == 459 || xo == 491) {
            operands(); out << Gpr(d) << "," << Gpr(a) << "," << Gpr(b);
        } else if (xo == 28 || xo == 60 || xo == 124 || xo == 412 || xo == 476 || xo == 284 || xo == 316 || xo == 444) {
            operands(); out << Gpr(a) << "," << Gpr(d) << "," << Gpr(b);
        } else if (xo == 339 || xo == 467) { }
        else { operands(); out << Gpr(d) << "," << Gpr(a) << "," << Gpr(b); }
        if (i.record && std::string(mnemonic).back() != '.') out << ".";
        break;
    }
    case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39:
    case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
        operands(); out << Gpr(d) << "," << simm << "(" << Gpr(a) << ")"; break;
    case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55:
        operands(); out << Fpr(d) << "," << simm << "(" << Gpr(a) << ")"; break;
    case 56: case 57: case 60: case 61: {
        const int32_t disp = (raw & 0x800u) ? int32_t(raw & 0xFFFu) - 0x1000 : int32_t(raw & 0xFFFu);
        operands(); out << Fpr(d) << "," << disp << "(" << Gpr(a) << ")," << ((raw >> 15) & 1u) << "," << ((raw >> 12) & 7u); break;
    }
    case 59: case 63: {
        const unsigned xo5 = (raw >> 1) & 31u;
        const bool aForm = (i.opcode == 59) || (xo5 == 18 || xo5 == 20 || xo5 == 21 || xo5 == 22 || xo5 == 23 || xo5 == 25 || xo5 == 28 || xo5 == 29 || xo5 == 30 || xo5 == 31);
        if (i.opcode == 63 && i.extendedOpcode == 38) { operands(); out << d; }
        else if (i.opcode == 63 && (i.extendedOpcode == 0 || i.extendedOpcode == 32)) { operands(); out << Crf((raw >> 23) & 7u) << "," << Fpr(a) << "," << Fpr(b); }
        else if (i.extendedOpcode == 711) { operands(); out << "0x" << std::hex << ((raw >> 17) & 0xFFu) << "," << Fpr(b); }
        else if (i.extendedOpcode == 583) { operands(); out << Fpr(d); }
        else if (i.opcode == 63 && !aForm) { operands(); out << Fpr(d) << "," << Fpr(b); }
        else if (xo5 == 22 || xo5 == 24) { operands(); out << Fpr(d) << "," << Fpr(b); }
        else if (xo5 == 25) { operands(); out << Fpr(d) << "," << Fpr(a) << "," << Fpr(c); }
        else if (xo5 == 23) { operands(); out << Fpr(d) << "," << Fpr(a) << "," << Fpr(c) << "," << Fpr(b); }
        else if (xo5 == 18 || xo5 == 20 || xo5 == 21) { operands(); out << Fpr(d) << "," << Fpr(a) << "," << Fpr(b); }
        else if (xo5 == 25) { operands(); out << Fpr(d) << "," << Fpr(a) << "," << Fpr(c); }
        else if (aForm) { operands(); out << Fpr(d) << "," << Fpr(a) << "," << Fpr(c) << "," << Fpr(b); }
        else { operands(); out << Fpr(d) << "," << Fpr(a) << "," << Fpr(b); }
        if (i.record) out << ".";
        break;
    }
    case 4: {
        const unsigned xo5 = (raw >> 1) & 31u;
        const bool aForm = xo5 == 10 || xo5 == 11 || xo5 == 12 || xo5 == 13 || xo5 == 14 || xo5 == 15 ||
            xo5 == 18 || xo5 == 20 || xo5 == 21 || xo5 == 23 || xo5 == 25 ||
            xo5 == 28 || xo5 == 29 || xo5 == 30 || xo5 == 31;
        const unsigned xo = aForm ? xo5 : i.extendedOpcode;
        if (xo == 40 || xo == 72 || xo == 136 || xo == 264) { operands(); out << Fpr(d) << "," << Fpr(b); }
        else if (xo == 0 || xo == 32 || xo == 64 || xo == 96) { operands(); out << Crf((raw >> 23) & 7u) << "," << Fpr(a) << "," << Fpr(b); }
        else if (xo == 528 || xo == 560 || xo == 592 || xo == 624) { operands(); out << Fpr(d) << "," << Fpr(a) << "," << Fpr(b); }
        else if (xo == 18 || xo == 20 || xo == 21) { operands(); out << Fpr(d) << "," << Fpr(a) << "," << Fpr(b); }
        else if (xo == 25) { operands(); out << Fpr(d) << "," << Fpr(a) << "," << Fpr(c); }
        else if (xo == 10 || xo == 11 || xo == 14 || xo == 15) { operands(); out << Fpr(d) << "," << Fpr(a) << "," << Fpr(c) << "," << Fpr(b); }
        else if (xo == 12 || xo == 13) { operands(); out << Fpr(d) << "," << Fpr(a) << "," << Fpr(c); }
        else { operands(); out << Fpr(d) << "," << Fpr(a) << "," << Fpr(c) << "," << Fpr(b); }
        if (i.record) out << ".";
        break;
    }
    case 17: break;
    default: break;
    }
    return out.str();
}
}
