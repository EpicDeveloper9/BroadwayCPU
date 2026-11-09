#include "cpu.hpp"
#include "executable_loader.hpp"
#include "video_frame.hpp"
#include "gx.hpp"
#include "exi.hpp"
#include "disc_image.hpp"
#include "wii_boot.hpp"
#include "wii_firmware.hpp"
#include "arm_elf.hpp"
#include "starlet_hardware.hpp"

#include <bit>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
unsigned g_checks = 0;

void Check(bool condition, const std::string& message) {
    ++g_checks;
    if (!condition) throw std::runtime_error(message);
}

void Equal(uint32_t actual, uint32_t expected, const std::string& message) {
    Check(actual == expected, message);
}

void EqualFloat(float actual, float expected, const std::string& message) {
    Check(std::fabs(actual - expected) < 0.00001f, message);
}

uint32_t D(uint8_t opcode, uint8_t rt, uint8_t ra, uint16_t immediate) {
    return (uint32_t(opcode) << 26) | (uint32_t(rt) << 21) |
        (uint32_t(ra) << 16) | immediate;
}

uint32_t X(uint8_t rt, uint8_t ra, uint8_t rb, uint16_t xo, bool record = false) {
    return (31u << 26) | (uint32_t(rt) << 21) | (uint32_t(ra) << 16) |
        (uint32_t(rb) << 11) | (uint32_t(xo) << 1) | uint32_t(record);
}

uint32_t SPR(uint16_t xo, uint8_t reg, uint16_t spr) {
    return (31u << 26) | (uint32_t(reg) << 21) |
        (uint32_t(spr & 31u) << 16) | (uint32_t(spr >> 5) << 11) |
        (uint32_t(xo) << 1);
}

double Pair(float ps0, float ps1) {
    const uint64_t bits = (uint64_t(std::bit_cast<uint32_t>(ps0)) << 32) |
        std::bit_cast<uint32_t>(ps1);
    return std::bit_cast<double>(bits);
}

float Pair0(double pair) {
    return std::bit_cast<float>(uint32_t(std::bit_cast<uint64_t>(pair) >> 32));
}

float Pair1(double pair) {
    return std::bit_cast<float>(uint32_t(std::bit_cast<uint64_t>(pair)));
}

void PutBE16(std::vector<uint8_t>& data, size_t offset, uint16_t value) {
    data[offset] = uint8_t(value >> 8); data[offset + 1] = uint8_t(value);
}

void PutBE32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    data[offset] = uint8_t(value >> 24); data[offset + 1] = uint8_t(value >> 16);
    data[offset + 2] = uint8_t(value >> 8); data[offset + 3] = uint8_t(value);
}

void TestMemoryAndInteger() {
    Broadway::CPU cpu(32u * 1024u * 1024u);
    cpu.memory.Write32(0x80000000u, 0x12345678u);
    Equal(cpu.memory.Read32(0), 0x12345678u, "Big-endian MEM1 alias read");
    Equal(cpu.memory.Read32(0xC0000000u), 0x12345678u, "Uncached MEM1 alias read");

    cpu.memory.Write32(0, D(14, 3, 0, uint16_t(-2)));
    cpu.Step();
    Equal(cpu.registers.GPR[3], 0xFFFFFFFEu, "addi sign-extends its immediate");
    Equal(cpu.registers.PC, 4, "Instruction step advances PC by four");

    cpu.registers.PC = 4;
    cpu.registers.GPR[1] = 0xFFFFFFFFu;
    cpu.memory.Write32(4, D(12, 2, 1, 1));
    cpu.Step();
    Equal(cpu.registers.GPR[2], 0, "addic wraps to 32 bits");
    Check(cpu.registers.GetXERCarry(), "addic sets carry");

    cpu.registers.GPR[3] = 0xFFFFFFFFu;
    cpu.registers.GPR[2] = 0;
    cpu.registers.SetXERCarry(true);
    cpu.registers.PC = 4;
    cpu.memory.Write32(4, X(1, 3, 2, 138)); // adde r3,r1,r2
    cpu.Step();
    Equal(cpu.registers.GPR[1], 0, "adde includes XER carry-in");
    Check(cpu.registers.GetXERCarry(), "adde writes the unsigned carry-out");

    cpu.registers.GPR[3] = 0x7FFFFFFFu;
    cpu.registers.GPR[2] = 0;
    cpu.registers.SetXERCarry(true);
    cpu.registers.PC = 4;
    cpu.memory.Write32(4, X(1, 3, 2, 138, true) | 0x400u); // addeo.
    cpu.Step();
    Equal(cpu.registers.GPR[1], 0x80000000u, "adde with OE produces the wrapped sum");
    Check(cpu.registers.GetXEROverflow(), "adde with OE sets signed overflow");
    Check(cpu.registers.GetXERSummaryOverflow(), "adde with OE sets sticky summary overflow");
    Equal(cpu.registers.GetCRField(0), 9u, "record-form adde writes LT and SO to CR0");

    cpu.registers.PC = 8;
    cpu.registers.GPR[4] = 0xF0F0AA55u;
    cpu.registers.GPR[5] = 0x0FF00FF0u;
    cpu.registers.GPR[6] = 0xFFFFFFFFu;
    cpu.memory.Write32(8, X(4, 6, 5, 28)); // and r6,r4,r5
    cpu.Step();
    Equal(cpu.registers.GPR[6], 0x00F00A50u, "and reads RS/RB and writes RA");
    Equal(cpu.registers.GPR[4], 0xF0F0AA55u, "and preserves RS");

    cpu.registers.PC = 12;
    cpu.registers.GPR[7] = 0xFFFFFFFFu;
    cpu.memory.Write32(12, X(7, 8, 9, 922)); // extsh r8,r7
    cpu.Step();
    Equal(cpu.registers.GPR[8], 0xFFFFFFFFu, "extsh sign-extends RS into RA");

    cpu.registers.PC = 16;
    cpu.registers.GPR[1] = 0;
    cpu.memory.Write32(16, D(11, 0, 1, 1)); // cmpwi cr0,r1,1
    cpu.Step();
    Equal(cpu.registers.GetCRField(0) >> 3, 1, "Signed immediate compare reports less-than");

    cpu.registers.PC = 20;
    cpu.memory.Write32(20, 59u << 26); // reserved single-precision FP XO
    bool unsupported = false;
    try { cpu.Step(); }
    catch (const Broadway::CPUException& e) { unsupported = e.Kind() == Broadway::ExceptionKind::UnimplementedInstruction; }
    Check(unsupported, "Unsupported encoding is distinct from invalid encoding");
    Equal(cpu.registers.PC, 20, "Faulting instruction preserves its PC");

    cpu.registers.PC = 24;
    cpu.memory.Write32(24, 0);
    bool invalid = false;
    try { cpu.Step(); }
    catch (const Broadway::CPUException& e) { invalid = e.Kind() == Broadway::ExceptionKind::InvalidInstruction; }
    Check(invalid, "Reserved primary opcode is an invalid instruction");

    bool aliasOverflow = false;
    try { cpu.memory.Read32(0x817FFFFFu); }
    catch (const std::out_of_range&) { aliasOverflow = true; }
    Check(aliasOverflow, "MEM1 alias boundary is enforced");

    cpu.memory.Write32(0xCC00680Cu, 0x00000021u);
    Equal(cpu.memory.Read32(0x0C00680Cu), 0x00000021u,
          "EXI control TSTART remains pending until an EXI transfer completes");
    Equal(cpu.memory.Read16(0xCC00680Eu), 0x0021u,
          "EXI control register lower half is consistent with 32-bit reads");
    Equal(cpu.memory.Read8(0x0C00680Fu), 0x21u,
          "EXI control register byte reads expose the same big-endian lane");
    cpu.memory.Write8(0xCC00680Fu, 0x03u);
    Equal(cpu.memory.Read32(0x0C00680Cu), 0x00000003u,
          "EXI control register byte writes update the aliased register state");
    cpu.memory.Write32(0xCD00680Cu, 0x00000015u);
    Equal(cpu.memory.Read32(0x0D00680Cu), 0x00000015u,
          "0xCD and 0x0D MMIO aliases share their own address bank");
    Equal(cpu.memory.Read32(0xCC00680Cu), 0x00000003u,
          "distinct 0x0C and 0x0D MMIO banks do not alias each other");
    bool mmioBoundary = false;
    try { (void)cpu.memory.Read16(0xCC007FFFu); }
    catch (const std::out_of_range&) { mmioBoundary = true; }
    Check(mmioBoundary, "MMIO accesses cannot cross the end of a mapped bank");
    cpu.memory.Clear();
    Equal(cpu.memory.Read32(0xCC00680Cu), 0,
          "Memory reset clears EXI control register state");
    Equal(cpu.memory.Read32(0x0D00680Cu), 0,
          "Memory reset clears the separate D-bank register state");
}

void TestEXIControllerRegisters() {
    Broadway::Memory memory(4096);
    Broadway::EXIController exi;
    memory.AttachMmioDevice(&exi);

    Check(!exi.Handles(0xCD00683Cu, 4),
          "EXI does not claim the unmapped tail of its address window");
    Equal(memory.Read32(0xCD00680Cu), 0,
          "EXI channel 0 control register resets with TSTART clear");
    memory.Write32(0xCD006800u, 0x000001A0u);
    Equal(memory.Read32(0x0D006800u), 0x000001A0u,
          "EXI status is visible through the physical alias");
    Equal(memory.Read32(0xCD00680Cu), 0,
          "EXI channel control and status registers are distinct");

    memory.Write32(0xCD00680Cu, 0x00000015u);
    Equal(memory.Read32(0x0D00680Cu), 0x00000015u,
          "An EXI transfer request remains pending without a modeled selected device");
    Equal(exi.TransferStartRequests(), 1u, "EXI counts an explicit TSTART write");
    Equal(exi.TransfersCompleted(), 0u,
          "EXI does not fabricate completion for an unmodeled device transfer");
    Equal(exi.MmioReads(), 4u, "EXI counts reads from its register block");
    Equal(exi.MmioWrites(), 2u, "EXI counts writes to its register block");

    Equal(memory.Read16(0xCD00680Eu), 0x0015u,
          "EXI control halfword read preserves big-endian register lanes");
    Equal(memory.Read8(0x0D00680Fu), 0x15u,
          "EXI control byte read selects the least-significant lane");
    memory.Write8(0xCD00680Fu, 0x02u);
    Equal(memory.Read32(0xCD00680Cu), 0x00000002u,
          "EXI control byte write updates only the addressed lane");

    memory.Write32(0xCC00680Cu, 0x00000003u);
    Equal(memory.Read32(0xCC00680Cu), 0x00000003u,
          "GameCube-compatible C-bank storage remains separate from Wii D-bank EXI");
    Equal(memory.Read32(0xCD00680Cu), 0x00000002u,
          "C-bank writes do not alter the Wii D-bank EXI control register");

    memory.Clear();
    Equal(exi.MmioReads(), 0u, "EXI reset clears access counters");
    Equal(exi.TransferStartRequests(), 0u, "EXI reset clears transfer-start accounting");
    Equal(memory.Read32(0xCD00680Cu), 0,
          "Memory reset resets attached EXI controller registers");
}

void TestMSRInstructions() {
    Broadway::CPU cpu(4096);
    cpu.registers.MSR = 0x12345678u;
    const uint32_t mfmsr = X(7, 0, 0, 83);
    const auto decodedMfmsr = Broadway::Decoder::Decode(mfmsr);
    Check(decodedMfmsr.recognized && decodedMfmsr.dispatchable && decodedMfmsr.implemented,
          "mfmsr encoding is decoded and dispatched");
    Check(Broadway::Decoder::Decode(mfmsr | (1u << 11)).type == Broadway::InstructionType::Invalid,
          "mfmsr with a reserved operand bit is rejected");
    cpu.memory.Write32(0, mfmsr); // mfmsr r7
    cpu.Step();
    Equal(cpu.registers.GPR[7], 0x12345678u, "mfmsr copies the current MSR to its destination");
    Equal(cpu.registers.MSR, 0x12345678u, "mfmsr does not modify MSR");
    Equal(cpu.registers.PC, 4, "Successful mfmsr advances PC once");

    cpu.registers.MSR = 1u << 17; // MSR[PR]
    cpu.registers.PC = 0;
    cpu.registers.GPR[7] = 0xA5A5A5A5u;
    bool privilegeFault = false;
    try { cpu.Step(); }
    catch (const Broadway::CPUException& e) {
        privilegeFault = e.Kind() == Broadway::ExceptionKind::Program;
    }
    Check(privilegeFault, "User-mode mfmsr raises a program exception");
    Equal(cpu.registers.PC, 0, "Privileged mfmsr fault leaves PC on the faulting instruction");
    Equal(cpu.registers.GPR[7], 0xA5A5A5A5u, "Faulting mfmsr leaves destination unchanged");

    const uint32_t mtmsr = X(8, 0, 0, 146);
    const auto decodedMtmsr = Broadway::Decoder::Decode(mtmsr);
    Check(decodedMtmsr.recognized && decodedMtmsr.dispatchable && decodedMtmsr.implemented,
          "mtmsr encoding is decoded and dispatched");
    Check(Broadway::Decoder::Decode(mtmsr | (1u << 11)).type == Broadway::InstructionType::Invalid,
          "mtmsr with a reserved operand bit is rejected");
    cpu.registers.MSR = 0;
    cpu.registers.GPR[8] = 0x90012345u;
    cpu.registers.PC = 4;
    cpu.memory.Write32(4, mtmsr);
    cpu.Step();
    Equal(cpu.registers.MSR, 0x90012345u, "mtmsr writes the source register to MSR");
    Equal(cpu.registers.PC, 8, "Successful mtmsr advances PC");

    cpu.registers.MSR = Broadway::CPURegisters::MSR_PR | 0x1234u;
    cpu.registers.GPR[8] = 0xFFFFFFFFu;
    cpu.registers.PC = 4;
    bool mtmsrFault = false;
    try { cpu.Step(); }
    catch (const Broadway::CPUException& e) { mtmsrFault = e.Kind() == Broadway::ExceptionKind::Program; }
    Check(mtmsrFault, "User-mode mtmsr raises a program exception");
    Equal(cpu.registers.MSR, Broadway::CPURegisters::MSR_PR | 0x1234u, "Faulting mtmsr preserves MSR");
    Equal(cpu.registers.GPR[8], 0xFFFFFFFFu, "Faulting mtmsr preserves source register");
    Equal(cpu.registers.PC, 4, "Faulting mtmsr preserves the faulting PC");

    const uint32_t rfi = (19u << 26) | (50u << 1);
    const auto decodedRfi = Broadway::Decoder::Decode(rfi);
    Check(decodedRfi.recognized && decodedRfi.dispatchable && decodedRfi.implemented,
          "Canonical rfi encoding is decoded and dispatched");
    Check(Broadway::Decoder::Decode(rfi | 1u).type == Broadway::InstructionType::Invalid,
          "rfi with reserved Rc set is rejected");
    cpu.registers.MSR = 0x0000ABCDu;
    cpu.registers.SRR0 = 0x100u;
    cpu.registers.SRR1 = 0x80050000u;
    cpu.registers.PC = 8;
    cpu.memory.Write32(8, rfi);
    cpu.Step();
    Equal(cpu.registers.MSR, 0x80050000u, "rfi restores MSR from SRR1");
    Equal(cpu.registers.PC, 0x100u, "rfi resumes at SRR0");

    // SRR0/SRR1 are supervisor SPRs, encoded with the split SPR field.
    const uint32_t mfsrr0 = SPR(339, 4, 26);
    const uint32_t mtsrr1 = SPR(467, 5, 27);
    auto decodedMfsrr0 = Broadway::Decoder::Decode(mfsrr0);
    auto decodedMtsrr1 = Broadway::Decoder::Decode(mtsrr1);
    Check(decodedMfsrr0.recognized && decodedMfsrr0.implemented && decodedMfsrr0.dispatchable,
          "mfspr SRR0 encoding is decoded and executable");
    Check(decodedMtsrr1.recognized && decodedMtsrr1.implemented && decodedMtsrr1.dispatchable,
          "mtspr SRR1 encoding is decoded and executable");
    cpu.registers.MSR = 0;
    cpu.registers.SRR0 = 0x3456789Au;
    cpu.registers.GPR[5] = 0x89ABCDEFu;
    cpu.registers.PC = 12;
    cpu.memory.Write32(12, mfsrr0);
    cpu.Step();
    Equal(cpu.registers.GPR[4], 0x3456789Au, "mfspr reads SRR0 into RT");
    Equal(cpu.registers.PC, 16, "Successful mfspr advances PC");
    cpu.registers.PC = 16;
    cpu.memory.Write32(16, mtsrr1);
    cpu.Step();
    Equal(cpu.registers.SRR1, 0x89ABCDEFu, "mtspr writes SRR1 from RS");
    Equal(cpu.registers.PC, 20, "Successful mtspr advances PC");

    cpu.registers.MSR = Broadway::CPURegisters::MSR_PR | 0x44u;
    cpu.registers.PC = 12;
    cpu.registers.GPR[4] = 0xDEADBEEFu;
    cpu.memory.Write32(12, mfsrr0);
    bool srrReadFault = false;
    try { cpu.Step(); }
    catch (const Broadway::CPUException& e) { srrReadFault = e.Kind() == Broadway::ExceptionKind::Program; }
    Check(srrReadFault, "User-mode mfspr SRR0 raises a program exception");
    Equal(cpu.registers.PC, 12, "Faulting mfspr preserves its PC");
    Equal(cpu.registers.GPR[4], 0xDEADBEEFu, "Faulting mfspr preserves RT");
    cpu.registers.PC = 16;
    cpu.registers.SRR1 = 0x76543210u;
    cpu.memory.Write32(16, mtsrr1);
    bool srrWriteFault = false;
    try { cpu.Step(); }
    catch (const Broadway::CPUException& e) { srrWriteFault = e.Kind() == Broadway::ExceptionKind::Program; }
    Check(srrWriteFault, "User-mode mtspr SRR1 raises a program exception");
    Equal(cpu.registers.PC, 16, "Faulting mtspr preserves its PC");
    Equal(cpu.registers.SRR1, 0x76543210u, "Faulting mtspr preserves SRR1");

    // SRR0 is normally saved aligned by exception entry. If software restores
    // a malformed address, rfi must preserve it and the subsequent fetch faults.
    cpu.registers.MSR = 0;
    cpu.registers.SRR0 = 0x103u;
    cpu.registers.SRR1 = 0x1234u;
    cpu.registers.PC = 8;
    cpu.memory.Write32(8, rfi);
    cpu.Step();
    Equal(cpu.registers.PC, 0x103u, "rfi preserves exact SRR0 value");
    bool badReturnAlignment = false;
    try { cpu.Step(); }
    catch (const Broadway::CPUException& e) { badReturnAlignment = e.Kind() == Broadway::ExceptionKind::Alignment; }
    Check(badReturnAlignment, "Unaligned rfi return address faults on the next instruction fetch");
    Equal(cpu.registers.PC, 0x103u, "Unaligned return fault preserves return PC");

    cpu.registers.MSR = Broadway::CPURegisters::MSR_PR | 0x55u;
    cpu.registers.SRR0 = 0x200u;
    cpu.registers.SRR1 = 0xAAu;
    cpu.registers.PC = 8;
    bool rfiFault = false;
    try { cpu.Step(); }
    catch (const Broadway::CPUException& e) { rfiFault = e.Kind() == Broadway::ExceptionKind::Program; }
    Check(rfiFault, "User-mode rfi raises a program exception");
    Equal(cpu.registers.PC, 8, "Faulting rfi preserves PC");
    Equal(cpu.registers.MSR, Broadway::CPURegisters::MSR_PR | 0x55u, "Faulting rfi preserves MSR");
    Equal(cpu.registers.SRR0, 0x200u, "Faulting rfi preserves SRR0");
    Equal(cpu.registers.SRR1, 0xAAu, "Faulting rfi preserves SRR1");
}

void TestSegmentRegisterInstructions() {
    Broadway::CPU cpu(4096);
    cpu.registers.GPR[3] = 0xAABBCCDDu;
    const uint32_t mtsr = X(3, 5, 0, 210);
    auto dMtsr = Broadway::Decoder::Decode(mtsr);
    Check(dMtsr.recognized && dMtsr.implemented && dMtsr.dispatchable,
          "mtsr is decoded and executable");
    cpu.memory.Write32(0, mtsr);
    cpu.Step();
    Equal(cpu.registers.SR[5], 0xAABBCCDDu, "mtsr writes selected segment register");

    cpu.registers.PC = 4;
    cpu.memory.Write32(4, X(6, 5, 0, 595)); // mfsr r6,SR5
    cpu.Step();
    Equal(cpu.registers.GPR[6], 0xAABBCCDDu, "mfsr reads selected segment register");

    cpu.registers.GPR[7] = 0x12345678u;
    cpu.registers.GPR[8] = 0xB0000000u;
    cpu.registers.PC = 8;
    cpu.memory.Write32(8, X(7, 0, 8, 242)); // mtsrin r7,r8 => SR11
    cpu.Step();
    Equal(cpu.registers.SR[11], 0x12345678u, "mtsrin selects SR from the top effective-address nibble");
    cpu.registers.PC = 12;
    cpu.memory.Write32(12, X(9, 0, 8, 659)); // mfsrin r9,r8
    cpu.Step();
    Equal(cpu.registers.GPR[9], 0x12345678u, "mfsrin reads SR selected by RB");

    cpu.registers.MSR = Broadway::CPURegisters::MSR_PR;
    cpu.registers.GPR[3] = 0xFFFFFFFFu;
    cpu.registers.PC = 0;
    bool denied = false;
    try { cpu.Step(); }
    catch (const Broadway::CPUException& e) { denied = e.Kind() == Broadway::ExceptionKind::Program; }
    Check(denied, "User-mode mtsr raises a program exception");
    Equal(cpu.registers.SR[5], 0xAABBCCDDu, "Faulting mtsr leaves SR unchanged");
    Equal(cpu.registers.PC, 0, "Faulting mtsr preserves PC");
    Check(Broadway::Decoder::Decode(X(3, 16, 0, 210)).type == Broadway::InstructionType::Invalid,
          "mtsr rejects its reserved segment selector bit");
}

void TestTimeBaseInstructions() {
    Broadway::CPU cpu(4096);
    cpu.registers.TimeBase = 0x123456789ABCDEF0ull;
    cpu.registers.MSR = Broadway::CPURegisters::MSR_PR; // mftb is user-level
    const uint32_t mftbl = SPR(371, 10, 268);
    const uint32_t mftbu = SPR(371, 11, 269);
    cpu.memory.Write32(0, mftbl);
    cpu.Step();
    Equal(cpu.registers.GPR[10], 0x9ABCDEF0u, "mftb reads the low time-base word in user mode");
    cpu.registers.PC = 4;
    cpu.memory.Write32(4, mftbu);
    cpu.Step();
    Equal(cpu.registers.GPR[11], 0x12345678u, "mftb reads the high time-base word");

    cpu.registers.GPR[12] = 0x0BADF00Du;
    const uint32_t mtsprTbl = SPR(467, 12, 284);
    cpu.registers.PC = 8;
    cpu.memory.Write32(8, mtsprTbl);
    bool denied = false;
    try { cpu.Step(); }
    catch (const Broadway::CPUException& e) { denied = e.Kind() == Broadway::ExceptionKind::Program; }
    Check(denied, "User-mode TBL write raises a program exception");
    Equal(uint32_t(cpu.registers.TimeBase), 0x9ABCDEF0u, "Faulting TBL write preserves time-base state");

    cpu.registers.MSR = 0;
    cpu.registers.PC = 8;
    cpu.Step();
    Equal(uint32_t(cpu.registers.TimeBase), 0x0BADF00Du, "Supervisor mtspr writes TBL");
    Equal(uint32_t(cpu.registers.TimeBase >> 32), 0x12345678u, "TBL write preserves TBU");
}

void TestHardwareRegisterInstructions() {
    Broadway::CPU cpu(4096);
    cpu.registers.GPR[3] = 0xF00D1234u;
    cpu.memory.Write32(0, SPR(467, 3, 915)); // mtspr GQR3,r3
    cpu.Step();
    Equal(cpu.registers.GQR[3], 0xF00D1234u, "mtspr writes GQR3");

    cpu.registers.PC = 4;
    cpu.memory.Write32(4, SPR(339, 4, 915)); // mfspr r4,GQR3
    cpu.Step();
    Equal(cpu.registers.GPR[4], 0xF00D1234u, "mfspr reads GQR3");

    cpu.registers.GPR[5] = 0x87654321u;
    cpu.registers.PC = 8;
    cpu.memory.Write32(8, SPR(467, 5, 920)); // mtspr HID2,r5
    cpu.Step();
    Equal(cpu.registers.HID2, 0x87654321u, "mtspr writes HID2");

    cpu.registers.GPR[10] = 0x10203040u;
    cpu.registers.PC = 28;
    cpu.memory.Write32(28, SPR(467, 10, 1008)); // HID0
    cpu.Step();
    Equal(cpu.registers.HID0, 0x10203040u, "mtspr writes HID0 state");
    cpu.registers.PC = 32;
    cpu.memory.Write32(32, SPR(339, 11, 1008));
    cpu.Step();
    Equal(cpu.registers.GPR[11], 0x10203040u, "mfspr reads HID0 state");

    cpu.registers.GPR[6] = 0x11223344u;
    cpu.registers.PC = 12;
    cpu.memory.Write32(12, SPR(467, 6, 536)); // mtspr DBAT0U,r6
    cpu.Step();
    Equal(cpu.registers.DBAT[0][0], 0x11223344u, "mtspr stores a data BAT upper register");
    cpu.registers.PC = 16;
    cpu.memory.Write32(16, SPR(339, 7, 536));
    cpu.Step();
    Equal(cpu.registers.GPR[7], 0x11223344u, "mfspr reads a data BAT register");

    cpu.registers.GPR[8] = 0xCAFEBABEu;
    cpu.registers.PC = 20;
    cpu.memory.Write32(20, SPR(467, 8, 273)); // SPRG1
    cpu.Step();
    Equal(cpu.registers.SPRG[1], 0xCAFEBABEu, "mtspr writes SPRG1");
    cpu.registers.PC = 24;
    cpu.memory.Write32(24, SPR(339, 9, 273));
    cpu.Step();
    Equal(cpu.registers.GPR[9], 0xCAFEBABEu, "mfspr reads SPRG1");

    cpu.registers.GPR[10] = 0xCC00801Fu;
    cpu.registers.PC = 36;
    cpu.memory.Write32(36, SPR(467, 10, 921));
    cpu.Step();
    Equal(cpu.registers.WPAR, 0xCC008000u, "mtspr WPAR aligns the configured write-gather target");
    cpu.registers.PC = 40;
    cpu.memory.Write32(40, SPR(339, 11, 921));
    cpu.Step();
    Equal(cpu.registers.GPR[11], 0xCC008000u, "mfspr WPAR reads its address while the write-gather buffer is empty");
    Check(Broadway::Decoder::Format(Broadway::Decoder::Decode(SPR(339, 11, 921)),0) == "mfspr  r11,wpar",
          "WPAR access uses the normal mfspr opcode decoder");

    cpu.registers.GPR[12] = 0x92345678u;
    cpu.registers.PC = 44;
    cpu.memory.Write32(44, SPR(467, 12, 1017));
    cpu.Step();
    Equal(cpu.registers.L2CR,0x12345678u,"mtspr updates the Gekko L2CR software-visible state");
    cpu.registers.PC=48;
    cpu.memory.Write32(48,SPR(339,13,1017));
    cpu.Step();
    Equal(cpu.registers.GPR[13],0x12345678u,"mfspr reads the Gekko L2CR software-visible state");
    Check(Broadway::Decoder::Format(Broadway::Decoder::Decode(SPR(339,13,1017)),0) == "mfspr  r13,l2cr",
          "Disassembler names the Gekko L2CR special-purpose register");

    cpu.registers.GPR[14] = 0xA5000042u;
    cpu.registers.PC = 52;
    cpu.memory.Write32(52, SPR(467, 14, 952));
    cpu.Step();
    Equal(cpu.registers.MMCR0, 0xA5000042u, "mtspr writes the MMCR0 control register");
    cpu.registers.PC = 56;
    cpu.memory.Write32(56, SPR(339, 15, 952));
    cpu.Step();
    Equal(cpu.registers.GPR[15], 0xA5000042u, "mfspr reads the MMCR0 control register");
    Check(Broadway::Decoder::Format(Broadway::Decoder::Decode(SPR(467, 14, 952)), 0) == "mtspr  mmcr0,r14",
          "Disassembler names MMCR0");
    cpu.registers.GPR[14] = 0x5A123456u;
    cpu.registers.PC = 60;
    cpu.memory.Write32(60, SPR(467, 14, 956));
    cpu.Step();
    Equal(cpu.registers.MMCR1, 0x5A123456u, "mtspr writes the MMCR1 event selector state");
    cpu.registers.PC = 64;
    cpu.memory.Write32(64, SPR(339, 15, 956));
    cpu.Step();
    Equal(cpu.registers.GPR[15], 0x5A123456u, "mfspr reads the MMCR1 event selector state");
    cpu.registers.GPR[14] = 1234;
    cpu.registers.PC = 68;
    cpu.memory.Write32(68, SPR(467, 14, 953));
    cpu.Step();
    Equal(cpu.registers.PMC[0], 1234u, "mtspr writes the PMC1 counter register");
    cpu.registers.PC = 72;
    cpu.memory.Write32(72, SPR(339, 15, 953));
    cpu.Step();
    Equal(cpu.registers.GPR[15], 1234u, "mfspr reads the PMC1 counter register");
    cpu.registers.GPR[14] = 0x12340000u;
    cpu.registers.PC = 76;
    cpu.memory.Write32(76, SPR(467, 14, 1011));
    cpu.Step();
    Equal(cpu.registers.HID4, 0x12340000u, "mtspr writes the Broadway HID4 state register");
    cpu.registers.PC = 80;
    cpu.memory.Write32(80, SPR(339, 15, 1011));
    cpu.Step();
    Equal(cpu.registers.GPR[15], 0x12340000u, "mfspr reads the Broadway HID4 state register");

    cpu.registers.GPR[10] = 0xDEADBEEFu;
    cpu.registers.MSR = Broadway::CPURegisters::MSR_PR;
    cpu.registers.PC = 36;
    bool wparDenied = false;
    try { cpu.Step(); }
    catch (const Broadway::CPUException& e) { wparDenied = e.Kind() == Broadway::ExceptionKind::Program; }
    Check(wparDenied, "User-mode WPAR write raises a privileged program exception");
    Equal(cpu.registers.WPAR, 0xCC008000u, "Faulting WPAR write preserves its state");
    Equal(cpu.registers.GPR[10], 0xDEADBEEFu, "Faulting WPAR write preserves the source register");

    cpu.registers.GPR[14] = 0xDEADBEEFu;
    cpu.registers.PC = 52;
    bool mmcrDenied = false;
    try { cpu.Step(); }
    catch (const Broadway::CPUException& e) { mmcrDenied = e.Kind() == Broadway::ExceptionKind::Program; }
    Check(mmcrDenied, "User-mode MMCR0 write raises a privileged program exception");
    Equal(cpu.registers.MMCR0, 0xA5000042u, "Faulting MMCR0 write preserves its state");

    cpu.registers.MSR = Broadway::CPURegisters::MSR_PR;
    cpu.registers.GPR[3] = 0xFFFFFFFFu;
    cpu.registers.PC = 0;
    bool denied = false;
    try { cpu.Step(); }
    catch (const Broadway::CPUException& e) { denied = e.Kind() == Broadway::ExceptionKind::Program; }
    Check(denied, "User-mode GQR access is privileged");
    Equal(cpu.registers.GQR[3], 0xF00D1234u, "Faulting GQR write preserves state");
}

void TestBranchesAndDebugger() {
    Broadway::CPU cpu(4096);
    cpu.memory.Write32(0, (18u << 26) | 8u); // b +8
    cpu.Step();
    Equal(cpu.registers.PC, 8, "Unconditional relative branch target");

    cpu.registers.PC = 16;
    cpu.memory.Write32(16, (18u << 26) | 8u | 2u | 1u); // ba +8, link
    cpu.Step();
    Equal(cpu.registers.PC, 8, "Absolute branch target");
    Equal(cpu.registers.LR, 20, "Branch-and-link saves next instruction address");

    cpu.registers.PC = 32;
    cpu.AddBreakpoint(32);
    cpu.Run(10);
    Check(cpu.BreakpointHit().has_value(), "Run stops at an active breakpoint");
    Equal(*cpu.BreakpointHit(), 32, "Breakpoint reports the stopped address");
    Check(cpu.RemoveBreakpoint(32), "Breakpoint can be removed");

    cpu.registers.PC = 36;
    cpu.memory.Write32(36, (19u << 26) | (150u << 1)); // isync
    cpu.Step();
    Equal(cpu.registers.PC, 40, "isync completes as an interpreter synchronization boundary");
}

void TestLoadsStoresAndExceptions() {
    Broadway::CPU cpu(4096);
    cpu.registers.GPR[1] = 0x100;
    cpu.registers.GPR[2] = 0xA1B2C3D4u;
    cpu.memory.Write32(0, D(36, 2, 1, 0)); // stw r2,0(r1)
    cpu.Step();
    Equal(cpu.memory.Read32(0x100), 0xA1B2C3D4u, "stw stores big-endian words");

    cpu.registers.PC = 4;
    cpu.registers.GPR[3] = 0;
    cpu.memory.Write32(4, D(32, 3, 1, 0));
    cpu.Step();
    Equal(cpu.registers.GPR[3], 0xA1B2C3D4u, "lwz loads big-endian words");

    cpu.registers.PC = 8;
    cpu.memory.Write32(8, D(32, 3, 1, 1));
    bool alignment = false;
    try { cpu.Step(); }
    catch (const Broadway::CPUException& e) { alignment = e.Kind() == Broadway::ExceptionKind::Alignment; }
    Check(alignment, "Unaligned word access raises alignment exception");
    Equal(cpu.registers.PC, 8, "Alignment fault leaves PC on faulting instruction");

    cpu.registers.PC = 12;
    cpu.registers.GPR[1] = 0xFFFFFFFCu;
    cpu.memory.Write32(12, D(34, 3, 1, 0));
    bool storage = false;
    try { cpu.Step(); }
    catch (const Broadway::CPUException& e) { storage = e.Kind() == Broadway::ExceptionKind::DataStorage; }
    Check(storage, "Unmapped byte access is reported as a data storage exception");

    cpu.registers.GPR[1] = 0x100;
    cpu.registers.GPR[2] = 4;
    cpu.registers.GPR[3] = 0xA1B2C3D4u;
    const uint32_t sthbrx = X(3, 1, 2, 918);
    const auto decodedSthbrx = Broadway::Decoder::Decode(sthbrx);
    Check(decodedSthbrx.recognized && decodedSthbrx.implemented && decodedSthbrx.dispatchable,
          "sthbrx encoding is decoded and reaches execution");
    cpu.registers.PC = 16;
    cpu.memory.Write32(16, sthbrx);
    cpu.Step();
    Equal(cpu.memory.Read16(0x104), 0xD4C3u, "sthbrx stores the low halfword with byte reversal");

    cpu.memory.Write32(0x108, 0x12345678u);
    cpu.registers.GPR[2] = 8;
    cpu.registers.PC = 20;
    cpu.memory.Write32(20, X(4, 1, 2, 534)); // lwbrx
    cpu.Step();
    Equal(cpu.registers.GPR[4], 0x78563412u, "lwbrx loads a word with byte reversal");
}

void TestIndexedFloatingPointMemory() {
    Broadway::CPU cpu(4096);
    cpu.registers.MSR |= Broadway::CPURegisters::MSR_FP;
    cpu.registers.GPR[1] = 0x100;
    cpu.registers.GPR[2] = 0x20;
    cpu.memory.Write32(0x120, std::bit_cast<uint32_t>(1.5f));
    const uint32_t lfsx = X(3, 1, 2, 535);
    auto decoded = Broadway::Decoder::Decode(lfsx);
    Check(decoded.recognized && decoded.implemented && decoded.dispatchable,
          "lfsx encoding has a floating-point execution path");
    cpu.memory.Write32(0, lfsx);
    cpu.Step();
    EqualFloat(float(cpu.registers.FPR[3]), 1.5f, "lfsx loads and widens an IEEE single");

    cpu.registers.FPR[4] = -2.25;
    cpu.registers.PC = 4;
    cpu.memory.Write32(4, X(4, 1, 2, 663)); // stfsx
    cpu.Step();
    Equal(cpu.memory.Read32(0x120), std::bit_cast<uint32_t>(-2.25f), "stfsx narrows and stores an IEEE single");

    cpu.registers.GPR[1] = 0x140;
    cpu.registers.GPR[2] = 8;
    cpu.memory.Write32(0x148, std::bit_cast<uint32_t>(4.5f));
    cpu.registers.PC = 8;
    cpu.memory.Write32(8, X(7, 1, 2, 567)); // lfsux
    cpu.Step();
    Equal(cpu.registers.GPR[1], 0x148u, "lfsux writes the effective address to RA");
    EqualFloat(float(cpu.registers.FPR[7]), 4.5f, "lfsux loads the indexed single value");

    cpu.registers.GPR[1] = 0x140;
    cpu.registers.GPR[2] = 12;
    cpu.registers.FPR[8] = 5.5;
    cpu.registers.PC = 12;
    cpu.memory.Write32(12, X(8, 1, 2, 695)); // stfsux
    cpu.Step();
    Equal(cpu.registers.GPR[1], 0x14Cu, "stfsux writes the effective address to RA");
    Equal(cpu.memory.Read32(0x14C), std::bit_cast<uint32_t>(5.5f), "stfsux stores the narrowed FPR value");

    cpu.registers.GPR[1] = 0x100;
    cpu.memory.Write64(0x124, std::bit_cast<uint64_t>(-6.25));
    cpu.registers.PC = 16;
    cpu.memory.Write32(16, D(50, 9, 1, 0x24)); // lfd
    cpu.Step();
    Check(cpu.registers.FPR[9] == -6.25, "lfd permits the architecture's four-byte alignment");

    cpu.registers.FPR[9] = 7.75;
    cpu.registers.PC = 20;
    cpu.memory.Write32(20, D(54, 9, 1, 0x24)); // stfd
    cpu.Step();
    Check(cpu.memory.Read64(0x124) == std::bit_cast<uint64_t>(7.75),
          "stfd stores a double at a four-byte-aligned address");

    cpu.registers.GPR[1] = 0x100;
    cpu.registers.GPR[2] = 0x28;
    cpu.memory.Write64(0x128, std::bit_cast<uint64_t>(3.125));
    cpu.registers.GPR[2] = 0x24;
    cpu.registers.PC = 24;
    cpu.memory.Write32(24, X(5, 1, 2, 599)); // lfdx
    cpu.Step();
    Check(std::bit_cast<uint64_t>(cpu.registers.FPR[5]) == cpu.memory.Read64(0x124),
          "lfdx permits a four-byte-aligned double load");

    cpu.registers.GPR[1] = 0x100;
    cpu.registers.GPR[2] = 0x2C;
    cpu.registers.FPR[10] = std::bit_cast<double>(0xFFF8000089ABCDEFu);
    cpu.registers.PC = 28;
    const uint32_t stfiwx = X(10, 1, 2, 983);
    const auto decodedStfiwx = Broadway::Decoder::Decode(stfiwx);
    Check(decodedStfiwx.recognized && decodedStfiwx.implemented &&
          decodedStfiwx.type == Broadway::InstructionType::LoadStore,
          "stfiwx is decoded into the indexed floating-point store path");
    cpu.memory.Write32(28, stfiwx);
    cpu.Step();
    Equal(cpu.memory.Read32(0x12C), 0x89ABCDEFu,
          "stfiwx stores the integer word held in the low word of an FPR");

    cpu.registers.GPR[1] = 0x100;
    cpu.registers.GPR[2] = 0x21;
    cpu.registers.FPR[6] = 9.0;
    cpu.registers.PC = 16;
    cpu.memory.Write32(16, X(6, 1, 2, 535));
    bool unaligned = false;
    try { cpu.Step(); }
    catch (const Broadway::CPUException& e) { unaligned = e.Kind() == Broadway::ExceptionKind::Alignment; }
    Check(unaligned, "lfsx reports an unaligned access");
    Equal(cpu.registers.PC, 16, "Faulting lfsx preserves PC");
    Check(cpu.registers.FPR[6] == 9.0, "Faulting lfsx preserves destination FPR");

    const uint32_t invalidRc = X(3, 1, 2, 535, true);
    Check(Broadway::Decoder::Decode(invalidRc).type == Broadway::InstructionType::Invalid,
          "lfsx rejects its reserved Rc bit");
}

void TestPairedSingle() {
    Broadway::CPU cpu(4096);
    cpu.registers.MSR |= Broadway::CPURegisters::MSR_FP;
    cpu.registers.HID2 = Broadway::CPURegisters::HID2_PSE | Broadway::CPURegisters::HID2_LSQE;
    cpu.registers.FPR[1] = Pair(1.25f, 2.5f);
    cpu.registers.FPR[2] = Pair(3.0f, 4.0f);
    cpu.registers.FPR[3] = Pair(8.0f, 9.0f);
    const uint32_t psSum0 = (4u << 26) | (4u << 21) | (1u << 16) |
        (2u << 11) | (3u << 6) | (10u << 1);
    cpu.memory.Write32(0, psSum0);
    cpu.Step();
    EqualFloat(Pair0(cpu.registers.FPR[4]), 5.25f, "ps_sum0 adds ps1 from its third operand");
    EqualFloat(Pair1(cpu.registers.FPR[4]), 9.0f, "ps_sum0 copies the requested second lane");

    cpu.registers.GPR[1] = 0x100;
    cpu.memory.Write32(0x100, std::bit_cast<uint32_t>(1.5f));
    cpu.memory.Write32(0x104, std::bit_cast<uint32_t>(-2.25f));
    const uint32_t psqLoad = (56u << 26) | (5u << 21) | (1u << 16);
    cpu.registers.PC = 4;
    cpu.memory.Write32(4, psqLoad);
    cpu.Step();
    EqualFloat(Pair0(cpu.registers.FPR[5]), 1.5f, "psq_l loads its first floating lane");
    EqualFloat(Pair1(cpu.registers.FPR[5]), -2.25f, "psq_l loads its second floating lane");

    cpu.registers.GQR[0] = (4u << 16); // unsigned-byte load type
    cpu.memory.Write8(0x110, 10);
    cpu.memory.Write8(0x111, 250);
    const uint32_t psqU8Load = (56u << 26) | (6u << 21) | (1u << 16) | 0x10u;
    cpu.registers.PC = 8;
    cpu.memory.Write32(8, psqU8Load);
    cpu.Step();
    EqualFloat(Pair0(cpu.registers.FPR[6]), 10.0f, "psq_l dequantizes unsigned bytes");
    EqualFloat(Pair1(cpu.registers.FPR[6]), 250.0f, "psq_l dequantizes both unsigned-byte lanes");

    cpu.registers.GQR[0] = 4u; // unsigned-byte store type
    cpu.registers.FPR[7] = Pair(-3.0f, 300.0f);
    const uint32_t psqStore = (60u << 26) | (7u << 21) | (1u << 16) | 0x20u;
    cpu.registers.PC = 12;
    cpu.memory.Write32(12, psqStore);
    cpu.Step();
    Equal(cpu.memory.Read8(0x120), 0, "psq_st clamps negative unsigned values to zero");
    Equal(cpu.memory.Read8(0x121), 255, "psq_st clamps unsigned overflow to the format maximum");
}

void TestMtfsb1() {
    constexpr uint32_t mtfsb1_29 = 0xFFA0004Cu;
    Broadway::CPU cpu(4096);
    auto decoded = Broadway::Decoder::Decode(mtfsb1_29);
    Check(decoded.recognized && decoded.validEncoding && decoded.implemented && decoded.dispatchable,
          "mtfsb1 recognizes and dispatches the exact RGHP52 encoding");
    Check(std::string(Broadway::Decoder::Name(decoded)) == "mtfsb1",
          "mtfsb1 reports its architectural mnemonic");
    Check(Broadway::Decoder::Format(decoded, 0) == "mtfsb1  29",
          "mtfsb1 disassembly prints FPSCR bit operand");
    const uint32_t reserved = mtfsb1_29 | (1u << 11);
    const auto invalid = Broadway::Decoder::Decode(reserved);
    Check(invalid.type == Broadway::InstructionType::Invalid && invalid.validityKnown && !invalid.validEncoding,
          "mtfsb1 rejects nonzero reserved fields");

    cpu.registers.MSR = Broadway::CPURegisters::MSR_FP;
    cpu.registers.FPSCR = 0x80000000u;
    cpu.registers.CR = 0x12345678u;
    cpu.registers.GPR[31] = 0xCAFEBABEu;
    cpu.registers.FPR[31] = -7.25;
    cpu.memory.Write32(0, mtfsb1_29);
    cpu.Step();
    Equal(cpu.registers.FPSCR, 0x80000004u, "mtfsb1 29 sets the low-order FPSCR NI bit");
    Equal(cpu.registers.PC, 4, "mtfsb1 advances PC by one instruction");
    Equal(cpu.registers.CR, 0x12345678u, "non-record mtfsb1 preserves the condition register");
    Equal(cpu.registers.GPR[31], 0xCAFEBABEu, "mtfsb1 has no GPR side effects");
    Check(cpu.registers.FPR[31] == -7.25, "mtfsb1 has no FPR side effects");

    cpu.registers.FPSCR = 0x80000000u;
    cpu.registers.CR = 0x12345678u;
    cpu.registers.PC = 4;
    cpu.memory.Write32(4, mtfsb1_29 | 1u);
    cpu.Step();
    Equal(cpu.registers.FPSCR, 0x80000004u, "record-form mtfsb1 sets NI");
    Equal(cpu.registers.GetCRField(1), 8u, "record-form mtfsb1 copies FPSCR field zero into CR1");
    Equal(cpu.registers.GetCRField(0), 1u, "record-form mtfsb1 preserves CR0");

    cpu.registers.FPSCR = 0;
    cpu.registers.PC = 8;
    cpu.memory.Write32(8, (63u << 26) | (1u << 21) | (38u << 1));
    cpu.Step();
    Equal(cpu.registers.FPSCR, 0u, "mtfsb1 cannot explicitly set FPSCR FEX");
    cpu.registers.PC = 12;
    cpu.memory.Write32(12, (63u << 26) | (2u << 21) | (38u << 1));
    cpu.Step();
    Equal(cpu.registers.FPSCR, 0u, "mtfsb1 cannot explicitly set FPSCR VX");

    cpu.registers.MSR = 0;
    cpu.registers.PC = 16;
    cpu.registers.FPSCR = 0x13579BDFu;
    cpu.registers.CR = 0x2468ACE0u;
    cpu.memory.Write32(16, mtfsb1_29);
    cpu.Step();
    Equal(cpu.registers.PC, 0x800u, "floating-point unavailable enters its architectural vector");
    Equal(cpu.registers.SRR0, 16u, "floating-point unavailable saves the faulting instruction for retry");
    Equal(cpu.registers.SRR1, 0u, "floating-point unavailable saves the prior MSR");
    Equal(cpu.registers.FPSCR, 0x13579BDFu, "faulting mtfsb1 preserves FPSCR");
    Equal(cpu.registers.CR, 0x2468ACE0u, "faulting mtfsb1 preserves CR");
}

void TestCacheInstructions() {
    Broadway::CPU cpu(4096);
    cpu.registers.GPR[1] = 0x100;
    cpu.registers.GPR[2] = 0x13;
    for (unsigned i = 0; i < 32; ++i) cpu.memory.Write8(0x100 + i, uint8_t(i + 1));
    const uint32_t dcbf = X(0, 1, 2, 86);
    const auto decoded = Broadway::Decoder::Decode(dcbf);
    Check(decoded.recognized && decoded.implemented && decoded.dispatchable,
          "dcbf has an execution path in the cacheless memory model");
    cpu.memory.Write32(0, dcbf);
    cpu.Step();
    Equal(cpu.registers.PC, 4, "dcbf completes and advances PC");
    Equal(cpu.memory.Read8(0x100), 1u, "dcbf preserves backing memory contents");

    const uint32_t dcbz = X(0, 1, 2, 1014);
    cpu.memory.Write32(4, dcbz);
    cpu.registers.PC = 4;
    cpu.Step();
    for (unsigned i = 0; i < 32; ++i)
        Equal(cpu.memory.Read8(0x100 + i), 0u, "dcbz zeroes the addressed 32-byte block");

    cpu.registers.GPR[1] = 0x1000;
    cpu.registers.GPR[2] = 0;
    cpu.registers.PC = 8;
    cpu.memory.Write32(8, X(0, 1, 2, 1014));
    bool storageFault = false;
    try { cpu.Step(); }
    catch (const Broadway::CPUException& e) {
        storageFault = e.Kind() == Broadway::ExceptionKind::DataStorage;
    }
    Check(storageFault, "dcbz reports data storage when its block is unmapped");
    Equal(cpu.registers.PC, 8, "faulting dcbz restores PC");
}

void TestDecoder() {
    const auto addi = Broadway::Decoder::Decode(D(14, 3, 4, uint16_t(-12)));
    Check(std::string(Broadway::Decoder::Name(addi)) == "addi", "Decoder supplies an exact mnemonic");
    Check(Broadway::Decoder::Format(addi, 0x80004000u) == "addi  r3,r4,-12", "Disassembler formats D-form operands");

    const auto branch = Broadway::Decoder::Decode((18u << 26) | 0x10u);
    Check(Broadway::Decoder::Format(branch, 0x80004000u) == "b  0x80004010", "Disassembler computes branch destinations");

    Check(Broadway::Decoder::Format(Broadway::Decoder::Decode(SPR(339, 4, 915)), 0) == "mfspr  r4,gqr3",
          "Disassembler names GQR special-purpose registers");

    const auto andInstruction = Broadway::Decoder::Decode(X(4, 6, 5, 28));
    Check(std::string(Broadway::Decoder::Name(andInstruction)) == "and", "Decoder names X-form logical instructions");
}

void TestVideoFrames() {
    const auto diagnostic0 = Broadway::CreateDiagnosticVideoFrame(640, 480, 0);
    const auto diagnostic1 = Broadway::CreateDiagnosticVideoFrame(640, 480, 1);
    Check(diagnostic0.Width() == 640 && diagnostic0.Height() == 480 &&
          diagnostic0.Format() == Broadway::VideoPixelFormat::BGRA8,
          "Diagnostic video producer creates a presentation-ready BGRA frame");
    const size_t redBar = (size_t(40) * diagnostic0.RowPitch()) + size_t(500) * 4;
    Check(diagnostic0.Data()[redBar] == 0 && diagnostic0.Data()[redBar + 1] == 0 &&
          diagnostic0.Data()[redBar + 2] == 255,
          "Diagnostic video contains distinct saturated color bars");
    const size_t gradientLeft = size_t(300) * diagnostic0.RowPitch() + size_t(10) * 4;
    const size_t gradientRight = size_t(300) * diagnostic0.RowPitch() + size_t(600) * 4;
    Check(diagnostic0.Data()[gradientLeft] != diagnostic0.Data()[gradientRight],
          "Diagnostic video contains a horizontal color gradient");
    const size_t markerLeft = size_t(350) * diagnostic0.RowPitch() + size_t(2) * 4;
    const size_t markerRight = size_t(350) * diagnostic0.RowPitch() + size_t(28) * 4;
    Check(diagnostic0.Data()[markerLeft] == 255 && diagnostic1.Data()[markerLeft] != 255 &&
          diagnostic0.Data()[markerRight] != 255 && diagnostic1.Data()[markerRight] == 255,
          "Diagnostic video moving marker advances between generated frames");
    const auto convertedDiagnostic = Broadway::ConvertToBGRA8(diagnostic0);
    Check(convertedDiagnostic.bytes[redBar + 2] == 255,
          "Diagnostic color pattern survives the existing video conversion path");

    bool badDimensions = false;
    try { (void)Broadway::VideoFrame::Create(0, 1, Broadway::VideoPixelFormat::BGRA8, 0, {}); }
    catch (const std::invalid_argument&) { badDimensions = true; }
    Check(badDimensions, "Video frame rejects zero dimensions");

    bool badPitch = false;
    try { (void)Broadway::VideoFrame::Create(2, 1, Broadway::VideoPixelFormat::BGRA8, 7, std::vector<uint8_t>(8)); }
    catch (const std::invalid_argument&) { badPitch = true; }
    Check(badPitch, "Video frame rejects a pitch shorter than its active row");

    bool shortStorage = false;
    try { (void)Broadway::VideoFrame::Create(2, 2, Broadway::VideoPixelFormat::RGBA8, 8, std::vector<uint8_t>(15)); }
    catch (const std::invalid_argument&) { shortStorage = true; }
    Check(shortStorage, "Video frame validates pitch multiplied by height against storage");

    bool oddYuyv = false;
    try { (void)Broadway::VideoFrame::Create(1, 1, Broadway::VideoPixelFormat::YUYV422BT601Limited, 2, std::vector<uint8_t>(2)); }
    catch (const std::invalid_argument&) { oddYuyv = true; }
    Check(oddYuyv, "YUYV422 frame rejects odd width");

    auto rgba = Broadway::VideoFrame::Create(1, 1, Broadway::VideoPixelFormat::RGBA8, 4,
        {10, 20, 30, 40});
    const auto bgra = Broadway::ConvertToBGRA8(rgba);
    Check(bgra.bytes == std::vector<uint8_t>({30, 20, 10, 40}), "RGBA8 converts to BGRA8 without losing alpha");

    auto rgb565 = Broadway::VideoFrame::Create(1, 1, Broadway::VideoPixelFormat::RGB565BigEndian, 2,
        {0xF8, 0x00});
    const auto red = Broadway::ConvertToBGRA8(rgb565);
    Check(red.bytes == std::vector<uint8_t>({0, 0, 255, 255}), "Big-endian RGB565 converts to opaque BGRA red");

    auto yuyv = Broadway::VideoFrame::Create(2, 1, Broadway::VideoPixelFormat::YUYV422BT601Limited, 4,
        {16, 128, 235, 128});
    const auto yuv = Broadway::ConvertToBGRA8(yuyv);
    Check(yuv.bytes[0] == 0 && yuv.bytes[1] == 0 && yuv.bytes[2] == 0 && yuv.bytes[3] == 255,
          "Limited-range BT.601 YUYV black converts correctly");
    Check(yuv.bytes[4] == 255 && yuv.bytes[5] == 255 && yuv.bytes[6] == 255 && yuv.bytes[7] == 255,
          "Limited-range BT.601 YUYV white converts correctly");

    Broadway::VideoFrameMailbox mailbox;
    std::atomic<unsigned> notifications{0};
    mailbox.SetNotify([&] { notifications.fetch_add(1, std::memory_order_relaxed); });
    std::thread producer([&] {
        mailbox.Publish(Broadway::VideoFrame::Create(1, 1, Broadway::VideoPixelFormat::BGRA8, 4,
            {1, 2, 3, 255}));
    });
    producer.join();
    Equal(notifications.load(std::memory_order_relaxed), 1, "Frame mailbox wakes the window on producer thread");
    const auto first = mailbox.TakeLatest();
    Check(first && first->Data()[0] == 1, "Frame mailbox transfers the newest complete frame");
    mailbox.Publish(Broadway::VideoFrame::Create(1, 1, Broadway::VideoPixelFormat::BGRA8, 4,
        {9, 8, 7, 255}));
    const auto second = mailbox.TakeLatest();
    Check(second && second->Data()[0] == 9, "Frame mailbox retains the newest published image");
    Check(first->Data()[0] == 1, "Consumed frame storage remains valid after a newer frame arrives");
    Check(!mailbox.TakeLatest(), "Taking a frame empties the one-slot mailbox");
}

void FeedBP(Broadway::GXProcessor& gx, uint8_t reg, uint32_t value) {
    gx.FeedCommandByte(0x61);
    gx.FeedCommandByte(reg);
    gx.FeedCommandByte(uint8_t(value >> 16));
    gx.FeedCommandByte(uint8_t(value >> 8));
    gx.FeedCommandByte(uint8_t(value));
}

void FeedCP(Broadway::GXProcessor& gx, uint8_t reg, uint32_t value) {
    for (uint8_t byte : {uint8_t(0x08), reg, uint8_t(value >> 24),
                         uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)})
        gx.FeedCommandByte(byte);
}

void FeedXF(Broadway::GXProcessor& gx, uint16_t address,
            const std::vector<uint32_t>& values) {
    const uint32_t header = (uint32_t(values.size() - 1) << 16) | address;
    for (uint8_t byte : {uint8_t(0x10), uint8_t(header >> 24), uint8_t(header >> 16),
                         uint8_t(header >> 8), uint8_t(header)})
        gx.FeedCommandByte(byte);
    for (uint32_t value : values)
        for (uint8_t byte : {uint8_t(value >> 24), uint8_t(value >> 16),
                             uint8_t(value >> 8), uint8_t(value)})
            gx.FeedCommandByte(byte);
}

void TestGXDirectQuadRasterization() {
    Broadway::Memory memory(2u * 1024u * 1024u);
    Broadway::VideoFrameMailbox mailbox;
    Broadway::GXProcessor gx(memory, mailbox);

    FeedCP(gx, 0x50, 0x00002200);
    FeedCP(gx, 0x60, 0);
    FeedCP(gx, 0x70, 0x00016009);
    FeedCP(gx, 0x80, 0);
    FeedCP(gx, 0x90, 0);
    FeedXF(gx, 0, {
        std::bit_cast<uint32_t>(1.0f), 0, 0, 0,
        0, std::bit_cast<uint32_t>(1.0f), 0, 0,
        0, 0, std::bit_cast<uint32_t>(1.0f), 0
    });
    FeedXF(gx, 0x101A, {
        std::bit_cast<uint32_t>(320.0f), std::bit_cast<uint32_t>(-264.0f),
        std::bit_cast<uint32_t>(1.0f), std::bit_cast<uint32_t>(662.0f),
        std::bit_cast<uint32_t>(606.0f), std::bit_cast<uint32_t>(1.0f)
    });
    FeedXF(gx, 0x1020, {
        std::bit_cast<uint32_t>(1.0f), 0, std::bit_cast<uint32_t>(1.0f),
        0, std::bit_cast<uint32_t>(1.0f), 0, 1
    });
    FeedBP(gx, 0x20, (342u << 12) | 342u);
    FeedBP(gx, 0x21, ((342u + 639u) << 12) | (342u + 527u));

    auto vertex = [&gx](float x, float y, uint8_t r, uint8_t g, uint8_t b) {
        const uint32_t position[] = {
            std::bit_cast<uint32_t>(x), std::bit_cast<uint32_t>(y),
            std::bit_cast<uint32_t>(0.0f)
        };
        for (uint32_t value : position)
            for (uint8_t byte : {uint8_t(value >> 24), uint8_t(value >> 16),
                                 uint8_t(value >> 8), uint8_t(value)})
                gx.FeedCommandByte(byte);
        for (uint8_t byte : {r, g, b, uint8_t(255)}) gx.FeedCommandByte(byte);
    };
    gx.FeedCommandByte(0x80);
    gx.FeedCommandByte(0);
    gx.FeedCommandByte(4);
    vertex(-1.0f, -1.0f, 255, 0, 0);
    vertex(1.0f, -1.0f, 0, 255, 0);
    vertex(1.0f, 1.0f, 0, 0, 255);
    vertex(-1.0f, 1.0f, 255, 255, 0);
    Check(!gx.IsStalled(), "GX accepts a direct XYZ/F32 + RGBA8 quad");
    Equal(gx.PrimitivesSubmitted(), uint64_t(1), "GX assembles one guest quad");
    Check(gx.EfbPixelsWritten() > 300000,
          "GX subtracts the hardware scissor bias and rasterizes the full quad into the EFB");
    FeedBP(gx, 0x49, 0);
    FeedBP(gx, 0x4A, (527u << 10) | 639u);
    FeedBP(gx, 0x4B, 0x800);
    FeedBP(gx, 0x4D, 40);
    FeedBP(gx, 0x4E, 0x100);
    FeedBP(gx, 0x52, 1u << 14);
    const auto frame = mailbox.TakeLatest();
    Check(bool(frame), "GX rasterized output reaches the existing frame mailbox by EFB copy");
    Equal(frame->Width(), 640u, "GX quad copy publishes the configured frame width");
    Equal(frame->Height(), 528u, "GX quad copy publishes the configured frame height");
    Check(frame->Data()[size_t(264) * 1280 + 320 * 2] > 16,
          "GX rasterized color reaches the copied frame pixels");
}

void TestGXCommandIngressAndCopy() {
    Broadway::Memory memory(2u * 1024u * 1024u);
    Broadway::VideoFrameMailbox mailbox;
    Broadway::GXProcessor gx(memory, mailbox);
    memory.AttachMmioDevice(&gx);

    auto result = gx.FeedCommandByte(0x00);
    Check(result.accepted && result.kind == Broadway::GXCommandKind::Nop,
          "GX FIFO NOP command decodes as a one-byte command");
    result = gx.FeedCommandByte(0x48);
    Check(result.accepted && result.kind == Broadway::GXCommandKind::InvalidateVertexCache,
          "GX FIFO vertex-cache invalidation command is accepted without inventing geometry");
    result = gx.FeedCommandByte(0x08);
    Check(!result.accepted, "GX CP load begins as a multi-byte FIFO command");
    for (uint8_t byte : {uint8_t(0x30),uint8_t(0x12),uint8_t(0x34),uint8_t(0x56),uint8_t(0x78)})
        result = gx.FeedCommandByte(byte);
    Check(result.accepted && result.kind == Broadway::GXCommandKind::LoadCP,
          "GX CP load command decodes its register and big-endian value");
    for (uint8_t byte : {uint8_t(0x61),uint8_t(0x4D),uint8_t(0),uint8_t(0),uint8_t(1)})
        result = gx.FeedCommandByte(byte);
    Check(result.accepted && result.kind == Broadway::GXCommandKind::LoadBP,
          "GX BP load command decodes register writes");
    for (uint8_t byte : {uint8_t(0x10),uint8_t(0x00),uint8_t(0x00),uint8_t(0x00),uint8_t(0x02),
                         uint8_t(0x12),uint8_t(0x34),uint8_t(0x56),uint8_t(0x78)})
        result = gx.FeedCommandByte(byte);
    Check(result.accepted && result.kind == Broadway::GXCommandKind::LoadXF,
          "GX XF register-load command honors its 32-bit address and count header");
    for (uint8_t byte : {uint8_t(0x10),uint8_t(0x00),uint8_t(0x00),uint8_t(0x10),uint8_t(0x00),
                         uint8_t(0x00),uint8_t(0x00),uint8_t(0x00),uint8_t(0x3F)})
        result = gx.FeedCommandByte(byte);
    Check(result.accepted && result.kind == Broadway::GXCommandKind::LoadXF,
          "GX XF command accepts the libogc bootstrap register-load encoding");
    const uint64_t beforeMmio = gx.CommandsProcessed();
    memory.Write8(0xCC008000u,0x00);
    Equal(gx.CommandsProcessed(),beforeMmio+1,"CPU memory bus forwards FIFO port writes to the GX parser");
    memory.Write8(0x0C008000u,0x00);
    Equal(gx.CommandsProcessed(),beforeMmio+2,"Uncached physical FIFO alias reaches the same GX command ingress");
    Equal(gx.MmioWrites(),2u,"GX tracks FIFO accesses forwarded through the MMIO bus");
    memory.Write64(0xCC008000u, 0);
    Equal(gx.CommandsProcessed(),beforeMmio+10,
          "A paired 64-bit FIFO store is forwarded as two ordered big-endian words");
    Equal(gx.MmioWrites(),4u,"Each word of a paired FIFO store is recorded as a bus write");

    gx.SetEfbPixel(0,0,255,0,0);
    gx.SetEfbPixel(1,0,0,0,255);
    gx.SetEfbPixel(0,1,255,0,0);
    gx.SetEfbPixel(1,1,0,0,255);
    FeedBP(gx,0x49,0);
    FeedBP(gx,0x4A,(1u << 10) | 1u);
    FeedBP(gx,0x4B,0x800u); // physical 0x10000, in 32-byte units
    FeedBP(gx,0x4D,1u); // 16-pixel stride
    FeedBP(gx,0x4E,0x100u); // 1.0 vertical scale
    FeedBP(gx,0x52,1u << 14);
    Equal(gx.FramesCopied(),1u,"GX display-copy command sends one frame through the XFB path");
    const auto frame=mailbox.TakeLatest();
    Check(bool(frame),"GX display copy publishes an owned frame to the video mailbox");
    Equal(frame->Width(),2u,"XFB frame reports active copy width rather than memory stride");
    Equal(frame->Height(),2u,"XFB frame preserves the copied scan-line count");
    Equal(frame->RowPitch(),32u,"XFB frame retains its hardware memory stride");
    Equal(frame->Data()[0],uint8_t(82),"EFB red pixel converts to limited-range XFB luma");
    Equal(frame->Data()[1],uint8_t(165),"XFB conversion averages pair chroma");
    Equal(frame->Data()[2],uint8_t(41),"EFB blue pixel converts to limited-range XFB luma");
    Equal(memory.Read8(0x10000u),uint8_t(82),"GX copy writes the XFB bytes into Broadway memory");
    Equal(memory.Read16(0xCC000000u),uint16_t(12),"GX CP status is available through the MMIO bus");
    Equal(gx.MmioReads(),1u,"GX tracks MMIO reads");
    memory.Write16(0xCC000020u, 0x8100);
    memory.Write16(0x0C000022u, 0x1234);
    Equal(memory.Read16(0xCC000020u), uint16_t(0x8100),
          "GX CP FIFO base register retains its high halfword");
    Equal(memory.Read16(0x0C000022u), uint16_t(0x1234),
          "GX CP FIFO low register is accessible through the physical alias");
    memory.Write16(0xCC00100Au, 0x000F);
    Equal(memory.Read16(0xCC00100Au), uint16_t(0),
          "GX Pixel Engine interrupt status is cleared by write-one-to-clear bits");
    memory.Write16(0xCC001002u, 0xA55A);
    Equal(memory.Read16(0xCC001002u), uint16_t(0xA55A),
          "GX Pixel Engine alpha configuration retains its read-modify-write state");
    memory.Write16(0xCC001006u, 0x0023);
    Equal(memory.Read16(0xCC001006u), uint16_t(0x0023),
          "GX Pixel Engine alpha mode register retains its configuration");
    memory.Write16(0xCC001004u, 0x01A5);
    memory.Write16(0xCC001008u, 0x0005);
    Equal(memory.Read16(0xCC001004u), uint16_t(0x01A5),
          "GX Pixel Engine destination alpha register retains its value");
    Equal(memory.Read16(0xCC001008u), uint16_t(0x0005),
          "GX Pixel Engine alpha read register retains its mode");
    memory.Write16(0xCC000006u, 0xFFFF);
    Equal(memory.Read16(0xCC000006u), uint16_t(7),
          "GX CP performance selector retains only its three documented selection bits");
    bool unsupportedRegisterRead=false;
    try { (void)memory.Read16(0xCC000008u); }
    catch (const std::runtime_error&) { unsupportedRegisterRead=true; }
    Check(unsupportedRegisterRead,"Unimplemented GX register reads report unsupported behavior instead of fabricated values");
    Equal(gx.MmioReads(),10u,"GX counts unsupported register read attempts as MMIO activity");

    Broadway::GXProcessor unsupported(memory, mailbox);
    unsupported.FeedCommandByte(0x88);
    unsupported.FeedCommandByte(0);
    result=unsupported.FeedCommandByte(4);
    Check(!result.accepted && unsupported.IsStalled() && !unsupported.LastError().empty(),
          "Unsupported GX primitive topology is rejected with a diagnostic");
    Broadway::GXProcessor invalid(memory, mailbox);
    result=invalid.FeedCommandByte(0x7F);
    Check(!result.accepted && invalid.IsStalled() && !invalid.LastError().empty(),
          "Unknown GX FIFO opcode is rejected with a diagnostic");
    gx.Reset();
    Equal(gx.MmioReads(),0u,"GX reset clears the MMIO read diagnostic counter");
    Equal(gx.MmioWrites(),0u,"GX reset clears the MMIO write diagnostic counter");
}

void TestExecutableLoading() {
    Broadway::CPU cpu(32u * 1024u * 1024u);
    const std::vector<uint8_t> raw{0x38, 0x60, 0x00, 0x01};
    const auto rawResult = Broadway::ExecutableLoader::Load(cpu, raw, Broadway::ExecutableFormat::Raw, 0x80004000u);
    Equal(rawResult.entryPoint, 0x80004000u, "Raw loader uses configurable base address");
    Equal(cpu.memory.Read32(0x80004000u), 0x38600001u, "Raw loader copies bytes in order");

    std::vector<uint8_t> dol(0x104);
    PutBE32(dol, 0, 0x100); PutBE32(dol, 0x48, 0x80004000u); PutBE32(dol, 0x90, 4);
    PutBE32(dol, 0xD8, 0x80005000u); PutBE32(dol, 0xDC, 4); PutBE32(dol, 0xE0, 0x80004000u);
    dol[0x100] = 0x38; dol[0x101] = 0x60; dol[0x102] = 0; dol[0x103] = 2;
    const auto dolResult = Broadway::ExecutableLoader::Load(cpu, dol, Broadway::ExecutableFormat::Dol);
    Check(dolResult.formatName == "DOL", "DOL loader identifies format");
    Equal(cpu.memory.Read32(0x80004000u), 0x38600002u, "DOL loader loads text section");
    Equal(cpu.memory.Read32(0x80005000u), 0, "DOL loader clears BSS");

    std::vector<uint8_t> elf(88);
    elf[0] = 0x7F; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F'; elf[4] = 1; elf[5] = 2; elf[6] = 1;
    PutBE16(elf, 16, 2); PutBE16(elf, 18, 20); PutBE32(elf, 20, 1); PutBE32(elf, 24, 0x80004000u);
    PutBE32(elf, 28, 52); PutBE16(elf, 40, 52); PutBE16(elf, 42, 32); PutBE16(elf, 44, 1);
    PutBE32(elf, 52, 1); PutBE32(elf, 56, 84); PutBE32(elf, 60, 0x80004000u);
    PutBE32(elf, 68, 4); PutBE32(elf, 72, 8); PutBE32(elf, 76, 5); PutBE32(elf, 80, 4);
    elf[84] = 0x38; elf[85] = 0x60; elf[86] = 0; elf[87] = 3;
    const auto elfResult = Broadway::ExecutableLoader::Load(cpu, elf, Broadway::ExecutableFormat::Elf);
    Check(elfResult.formatName == "ELF32 PowerPC", "ELF loader identifies target architecture");
    Equal(cpu.memory.Read32(0x80004000u), 0x38600003u, "ELF loader maps executable segment");
    Equal(cpu.memory.Read32(0x80004004u), 0, "ELF loader clears the segment memory tail");

    bool rejected = false;
    try { Broadway::ExecutableLoader::Load(cpu, std::vector<uint8_t>{1,2,3}, Broadway::ExecutableFormat::Dol); }
    catch (const std::exception&) { rejected = true; }
    Check(rejected, "Malformed DOL is rejected");
    Equal(cpu.memory.Read32(0x80004000u), 0x38600003u, "Failed load leaves the prior executable intact");
}

void TestWiiDiscImageReader() {
    constexpr size_t partitionOffset = 0x50000;
    constexpr size_t partitionDataOffset = 0x8000;
    constexpr size_t partitionDataSize = 0x3000;
    const size_t dataStart = partitionOffset + partitionDataOffset;
    std::vector<uint8_t> image(dataStart + partitionDataSize);
    image[0] = 'R'; image[1] = 'G'; image[2] = 'H'; image[3] = 'P'; image[4] = '5'; image[5] = '2';
    image[6] = 0; image[7] = 1;
    PutBE32(image, 0x18, 0x5D1C9EA3u);
    PutBE32(image, 0x40000, 1);
    PutBE32(image, 0x40004, 0x10008);
    PutBE32(image, 0x40020, 0x14000);
    PutBE32(image, 0x40024, 0);
    image[0x61] = 1; // Unencrypted development image.
    PutBE32(image, partitionOffset + 0x2B8, uint32_t(partitionDataOffset / 4));
    PutBE32(image, partitionOffset + 0x2BC, uint32_t(partitionDataSize / 4));
    PutBE32(image, dataStart + 0x420, 0x2000 / 4);
    PutBE32(image, dataStart + 0x424, 0x100 / 4);
    PutBE32(image, dataStart + 0x428, 36 / 4);
    PutBE32(image, dataStart + 0x42C, 36 / 4);
    PutBE32(image, dataStart + 0x2440 + 0x10, 0x81200294u);
    PutBE32(image, dataStart + 0x2440 + 0x14, 0x100u);
    PutBE32(image, dataStart + 0x2440 + 0x18, 0x20u);
    image[dataStart + 0x2440 + 0x20] = 0xA5;
    PutBE32(image, dataStart + 0x100, 0x01000000u);
    PutBE32(image, dataStart + 0x108, 2);
    PutBE32(image, dataStart + 0x10C, 0);
    PutBE32(image, dataStart + 0x110, 0x1234);
    PutBE32(image, dataStart + 0x114, 0x20);
    const char fstName[] = "main.dol";
    std::copy(std::begin(fstName), std::end(fstName), image.begin() + dataStart + 0x118);
#ifdef _WIN32
    std::array<uint8_t, 16> commonKey{}, expectedTitleKey{};
    for (unsigned n = 0; n < 16; ++n) {
        commonKey[n] = uint8_t(n);
        expectedTitleKey[n] = uint8_t(0xF0u + n);
    }
    const std::array<uint8_t, 16> encryptedTitleKey{
        0x0F,0x8D,0x0A,0x73,0x8B,0xC8,0xA0,0xA8,
        0x5C,0x4A,0xF4,0xA9,0x5A,0x5F,0x46,0xF2};
    for (size_t n = 0; n < encryptedTitleKey.size(); ++n)
        image[partitionOffset + 0x1BF + n] = encryptedTitleKey[n];
    for (size_t n = 0; n < 8; ++n) image[partitionOffset + 0x1DC + n] = uint8_t(n + 1);
#endif
    const auto path = std::filesystem::temp_directory_path() / "broadwaycpu-disc-reader-test.iso";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    }
    {
        Broadway::WiiDiscImage disc(path);
        Check(disc.Header().gameId == "RGHP52", "Wii image reader extracts the six-byte game ID");
        Equal(disc.Header().revision, 1, "Wii image reader extracts the disc revision");
        Check(!disc.Header().encrypted, "Wii disc header exposes the unencrypted-data flag");
        Equal(uint32_t(disc.Partitions().size()), 1, "Wii image reader finds all declared partition entries");
        Equal(uint32_t(disc.Partitions()[0].offset), uint32_t(partitionOffset), "Partition table offset is scaled from words");
        Equal(disc.Partitions()[0].type, 0, "Wii game partition type is retained");
        const auto raw = disc.ReadRaw(0x18, 4);
        Equal(uint32_t(raw[0]), 0x5D, "Wii raw reads preserve on-disc byte ordering");
        const std::array<uint8_t, 16> unusedTitleKey{};
        const auto bootInfo = disc.ReadBootInformation(0, unusedTitleKey);
        Equal(bootInfo.dolOffset, 0x2000u, "Wii boot block reports the DOL's partition-relative offset");
        Equal(bootInfo.fstOffset, 0x100u, "Wii boot block reports the FST offset");
        Equal(bootInfo.apploaderEntry, 0x81200294u, "Apploader header reports its entry address");
        Equal(bootInfo.apploaderCodeSize, 0x100u, "Apploader header reports code size");
        Equal(bootInfo.apploaderTrailerSize, 0x20u, "Apploader header reports trailer size");
        const auto apploader = disc.ReadApploaderImage(0, bootInfo, unusedTitleKey);
        Equal(uint32_t(apploader.size()), 0x120u, "Apploader reader returns code plus trailer bytes");
        Equal(uint32_t(apploader.front()), 0xA5u, "Apploader reader starts immediately after its header");
        const auto fsEntries = disc.ReadFileSystem(0, bootInfo, unusedTitleKey);
        Equal(uint32_t(fsEntries.size()), 2, "Wii FST reader returns root and file entries");
        Check(fsEntries[1].path == "/main.dol" && fsEntries[1].offset == 0x1234 &&
              fsEntries[1].size == 0x20 && !fsEntries[1].directory,
              "Wii FST parser resolves file name, logical offset, and length");
#ifdef _WIN32
        const auto titleKey = disc.DeriveTitleKey(0, commonKey);
        Check(titleKey == expectedTitleKey, "Ticket title key is AES-CBC decrypted with title-ID IV");
#endif
        bool rejected = false;
        try { (void)disc.ReadRaw(disc.ImageSize(), 1); }
        catch (const std::exception&) { rejected = true; }
        Check(rejected, "Wii image reader rejects reads past image bounds");
    }
    std::filesystem::remove(path);
}
}

void TestSystemCallExceptionEntry() {
    Broadway::CPU cpu(32u * 1024u * 1024u);
    constexpr uint32_t sc = 0x44000002u;
    cpu.memory.Write32(0x80001000u, sc);
    cpu.registers.PC = 0x80001000u;
    cpu.registers.MSR = (1u << 17) | (1u << 15) | (1u << 13) |
                        (1u << 5) | (1u << 4) | (1u << 1) | (1u << 6);
    cpu.registers.GPR[0] = 0x3Cu;
    cpu.registers.GPR[3] = 0x12345678u;
    cpu.registers.LR = 0x87654320u;
    const auto decoded = Broadway::Decoder::Decode(sc);
    Check(decoded.recognized && decoded.validEncoding && decoded.implemented,
          "sc encoding is recognized, valid, and dispatched");
    cpu.Step();
    Equal(cpu.registers.PC, 0xFFF00C00u, "sc selects the high system-call exception vector when MSR[IP] is set");
    Equal(cpu.registers.SRR0, 0x80001004u, "sc saves the following instruction address in SRR0");
    Equal(cpu.registers.SRR1, (1u << 17) | (1u << 15) | (1u << 13) |
                              (1u << 5) | (1u << 4) | (1u << 1) | (1u << 6),
          "sc saves pre-exception MSR in SRR1");
    Equal(cpu.registers.MSR, 1u << 6, "sc clears interrupt, privilege, FP, translation, and recoverability state while preserving MSR[IP]");
    Equal(cpu.registers.GPR[0], 0x3Cu, "sc preserves syscall number register");
    Equal(cpu.registers.GPR[3], 0x12345678u, "sc preserves argument/result register");
    Equal(cpu.registers.LR, 0x87654320u, "sc preserves link register");
    Equal(uint32_t(cpu.InstructionsExecuted()), 1, "sc counts as an executed architectural instruction");

    cpu.Reset();
    cpu.memory.Write32(0x80001000u, sc);
    cpu.registers.PC = 0x80001000u;
    cpu.Step();
    Equal(cpu.registers.PC, 0x00000C00u, "sc selects the low system-call vector when MSR[IP] is clear");
}

void TestWiiBootLowMemory() {
    Broadway::CPU cpu(32u * 1024u * 1024u);
    std::array<uint8_t, 0x20> discHeader{};
    discHeader[0] = 'R'; discHeader[1] = 'G'; discHeader[2] = 'H'; discHeader[3] = 'P';
    discHeader[4] = '5'; discHeader[5] = '2'; discHeader[6] = 0; discHeader[7] = 1;
    discHeader[0x18] = 0x5D; discHeader[0x19] = 0x1C;
    discHeader[0x1A] = 0x9E; discHeader[0x1B] = 0xA3;
    Broadway::InitializeWiiBootMemory(cpu.memory, discHeader, 0x0F800000u, 0);
    Equal(cpu.memory.Read32(0x80000000u), 0x52474850u, "boot low memory receives the real disc ID header bytes");
    Equal(cpu.memory.Read32(0x80000018u), 0x5D1C9EA3u, "boot low memory preserves the disc's Wii layout magic");
    Equal(cpu.memory.Read32(0x80000020u), 0x0D15EA5Eu, "boot low memory receives the standard boot marker");
    Equal(cpu.memory.Read32(0x80000024u), 1u, "boot low memory receives the documented apploader ABI version");
    Equal(cpu.memory.Read32(0x80000028u), 0x01800000u, "boot low memory reports physical MEM1 size");
    Equal(cpu.memory.Read32(0x8000002Cu), 0x23u, "boot low memory reports the Wii production board model");
    Equal(cpu.memory.Read32(0x800000F0u), 0x01800000u, "boot low memory reports simulated MEM1 size");
    Equal(cpu.memory.Read32(0x800000F8u), 0x0E7BE2C0u, "boot low memory reports the documented bus clock");
    Equal(cpu.memory.Read32(0x800000FCu), 0x2B73A840u, "boot low memory reports the documented CPU clock");
    Equal(cpu.memory.Read32(0x80003194u), 0u, "boot low memory reports the selected data partition type");
    Equal(cpu.memory.Read32(0x80003198u), 0x0F800000u, "boot low memory reports the selected data partition offset");
    Equal(cpu.memory.Read32(0x80003184u), 0x80000000u, "boot low memory reports disc application type");
}

void TestWiiFirmwareInventory() {
    auto makeTmd = [](uint64_t systemVersion, uint64_t titleId, bool secondContent = false) {
        std::vector<uint8_t> bytes(0x140 + 0xA4 + (secondContent ? 0x48 : 0x24), 0);
        PutBE32(bytes, 0, 0x00010001);
        const size_t body = 0x140;
        auto put64 = [&](size_t p, uint64_t v) { PutBE32(bytes, p, uint32_t(v >> 32)); PutBE32(bytes, p + 4, uint32_t(v)); };
        put64(body + 0x44, systemVersion);
        put64(body + 0x4c, titleId);
        PutBE16(bytes, body + 0x9c, 1);
        PutBE16(bytes, body + 0x9e, secondContent ? 2 : 1);
        PutBE16(bytes, body + 0xa0, 0);
        PutBE32(bytes, body + 0xA4, 0x1234);
        PutBE16(bytes, body + 0xA8, 0);
        PutBE16(bytes, body + 0xAA, 1);
        put64(body + 0xAC, 4);
        if (secondContent) {
            PutBE32(bytes, body + 0xA4 + 0x24, 0x5678);
            PutBE16(bytes, body + 0xA4 + 0x28, 1);
            PutBE16(bytes, body + 0xA4 + 0x2A, 1);
            put64(body + 0xA4 + 0x2C, 8);
        }
        return bytes;
    };
    const uint64_t menuId = 0x0000000100000002ull;
    const uint64_t iosId = 0x000000010000003Aull;
    auto parsed = Broadway::WiiFirmwareInspector::ParseTmd(makeTmd(iosId, menuId));
    Check(parsed.titleId == menuId && parsed.systemVersion == iosId && parsed.bootIndex == 0,
          "TMD parser reads title, required IOS, and boot index from big-endian fields");
    Check(parsed.contents.size() == 1 && parsed.contents[0].id == 0x1234 && parsed.contents[0].size == 4,
          "TMD parser reads content records");

    bool rejected = false;
    try { (void)Broadway::WiiFirmwareInspector::ParseTmd(std::vector<uint8_t>(16, 0)); }
    catch (const std::exception&) { rejected = true; }
    Check(rejected, "TMD parser rejects unknown signature types");
    auto truncated = makeTmd(iosId, menuId); truncated.pop_back();
    rejected = false;
    try { (void)Broadway::WiiFirmwareInspector::ParseTmd(truncated); }
    catch (const std::exception&) { rejected = true; }
    Check(rejected, "TMD parser rejects truncated content tables");

    const auto root = std::filesystem::temp_directory_path() / "broadwaycpu-firmware-fixture";
    std::filesystem::remove_all(root);
    auto write = [](const std::filesystem::path& path, const std::vector<uint8_t>& data) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    };
    const auto menu = root / "title/00000001/00000002";
    const auto ios = root / "title/00000001/0000003a";
    write(menu / "content/title.tmd", makeTmd(iosId, menuId));
    write(menu / "content/00001234.app", {0});
    write(root / "ticket/00000001/00000002.tik", {0});
    write(ios / "content/title.tmd", makeTmd(0, iosId));
    write(ios / "content/00001234.app", {0});
    write(root / "ticket/00000001/0000003a.tik", {0});
    const auto inventory = Broadway::WiiFirmwareInspector::InspectNand(root);
    Check(inventory.requiredIosTitleId == iosId, "NAND inventory follows System Menu TMD's required IOS title ID");
    Check(inventory.requiredFilesPresent && inventory.iosBootContentPresent,
          "NAND inventory finds Menu and IOS boot content and ticket files");
    Check(inventory.iosContentFilesPresent == 1 && inventory.iosContentFilesTotal == 1,
          "NAND inventory counts all IOS title contents declared by its TMD");
    Check(inventory.status.find("boot runtime is not implemented") != std::string::npos,
          "NAND inventory reports that discovery does not implement system boot");
    write(ios / "content/title.tmd", makeTmd(0, iosId, true));
    const auto incomplete = Broadway::WiiFirmwareInspector::InspectNand(root);
    Check(!incomplete.requiredFilesPresent && incomplete.missingIosContentPaths.size() == 1,
          "NAND inventory reports missing non-boot IOS modules instead of treating kernel presence as complete");
    auto sharedTmd = makeTmd(0, iosId);
    constexpr size_t sharedRecord = 0x140 + 0xA4;
    PutBE16(sharedTmd, sharedRecord + 6, 0x8001);
    std::vector<uint8_t> sharedMap(28, 0);
    const char sharedName[] = "000000ab";
    for (size_t i = 0; i < 8; ++i) sharedMap[i] = uint8_t(sharedName[i]);
    for (size_t i = 0; i < 20; ++i) {
        const uint8_t hashByte = uint8_t(0xA0 + i);
        sharedTmd[sharedRecord + 16 + i] = hashByte;
        sharedMap[8 + i] = hashByte;
    }
    write(ios / "content/title.tmd", sharedTmd);
    write(root / "shared1/content.map", sharedMap);
    write(root / "shared1/000000ab.app", {0});
    const auto withShared = Broadway::WiiFirmwareInspector::InspectNand(root);
    Check(withShared.requiredFilesPresent && withShared.iosContentFilesPresent == 1,
          "NAND inventory resolves shared IOS content by its TMD SHA-1 in content.map");
    std::filesystem::remove_all(root);
}

void TestWiiSystemEnvironmentState() {
    const auto root = std::filesystem::path(__FILE__).parent_path();
    const auto env = Broadway::WiiSystemEnvironment::Inspect(root);
    Check(env.hasSystemMenuTmd && env.hasIosTmd && env.requiredIosTitleId == 0x0000000100000050ull,
          "NAND environment detection resolves the actual System Menu and required IOS title from the extracted Wii files");
    Check(env.bootStage == Broadway::WiiBootStage::Boot2Missing || env.bootStage == Broadway::WiiBootStage::BootFilesAvailable,
          "NAND environment classification distinguishes real title inventory from an implemented boot runtime");
    Check(env.status.find("boot runtime") != std::string::npos || env.status.find("boot2") != std::string::npos,
          "NAND environment exposes an honest status describing the remaining boot-chain gap");
}

void TestHomebrewDol() {
    const std::filesystem::path path = "Wii_Hello_World.dol";
    if (!std::filesystem::exists(path)) {
        std::cout << "Homebrew DOL not present; skipping.\n";
        return;
    }
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    Broadway::CPU cpu(32u * 1024u * 1024u);
    Broadway::VideoFrameMailbox mailbox;
    Broadway::GXProcessor gx(cpu.memory, mailbox);
    Broadway::EXIController exi;
    cpu.memory.AttachMmioDevice(&gx);
    cpu.memory.AddMmioDevice(&exi);
    const auto result = Broadway::ExecutableLoader::Load(cpu, bytes, Broadway::ExecutableFormat::Dol);
    cpu.memory.Write32(0xcc005004, 0x8000);
    std::cout << "DOL entry: 0x" << std::hex << result.entryPoint << " segments=" << std::dec << result.segmentCount << "\n";
    std::cout << "EXI initial CSR=0x" << std::hex << exi.Status(0)
              << " CR=0x" << exi.Control(0) << std::dec << "\n";
    try {
        for (int i = 0; i < 2000; ++i) {
            cpu.Run(100000);
            uint32_t raw = 0;
            try { raw = cpu.memory.Read32(cpu.registers.PC); } catch (...) {}
            if (i < 5 || i % 10 == 0)
                std::cout << "ran " << cpu.InstructionsExecuted() << " PC=0x" << std::hex << cpu.registers.PC
                          << " raw=0x" << raw << " r9=" << cpu.registers.GPR[9] << " r10=" << cpu.registers.GPR[10]
                          << " CTR=" << cpu.registers.CTR << " GX cmds=" << gx.CommandsProcessed() << "\n";
            if (cpu.halted) break;
        }
    } catch (const std::exception& e) {
        std::cout << "EXCEPTION after " << cpu.InstructionsExecuted() << " at 0x" << std::hex << cpu.registers.PC
                  << ": " << e.what() << "\n";
        std::cout << "  r3=0x" << cpu.registers.GPR[3] << " r4=0x" << cpu.registers.GPR[4] << " r5=0x" << cpu.registers.GPR[5]
                  << " r8=0x" << cpu.registers.GPR[8] << " r9=0x" << cpu.registers.GPR[9] << " r10=0x" << cpu.registers.GPR[10] << "\n";
        uint32_t raw = 0;
        try { raw = cpu.memory.Read32(cpu.registers.PC); } catch (...) {}
        std::cout << "  raw at PC: 0x" << std::hex << raw << " decoded=" << Broadway::Decoder::Name(Broadway::Decoder::Decode(raw)) << "\n";
    }
    std::cout << "final instructions=" << std::dec << cpu.InstructionsExecuted()
              << " PC=" << std::hex << cpu.registers.PC << " r2=" << cpu.registers.GPR[2]
              << " r3=" << cpu.registers.GPR[3] << " r4=" << cpu.registers.GPR[4]
              << " r6=" << cpu.registers.GPR[6] << " r9=" << cpu.registers.GPR[9]
              << " r10=" << cpu.registers.GPR[10] << " r11=" << cpu.registers.GPR[11]
              << " LR=" << cpu.registers.LR << " MSR=" << cpu.registers.MSR << " CTR=" << cpu.registers.CTR << "\n";
    for (uint32_t a = 0x8001b7a0; a <= 0x8001b820; a += 4) {
        uint32_t raw = 0; try { raw = cpu.memory.Read32(a); } catch (...) {}
        std::cout << "  " << std::hex << a << ": 0x" << raw << "\n";
    }
    std::cout << "  r7=" << std::hex << cpu.registers.GPR[7] << " r8=" << cpu.registers.GPR[8] << " r9=" << cpu.registers.GPR[9] << " r10=" << cpu.registers.GPR[10] << " r11=" << cpu.registers.GPR[11] << " r12=" << cpu.registers.GPR[12] << "\n";
    for (uint32_t a = 0x8001b7c0; a <= 0x8001b810; a += 4) {
        uint32_t raw = 0; try { raw = cpu.memory.Read32(a); } catch (...) {}
        std::cout << "  " << std::hex << a << ": 0x" << raw << "\n";
    }
    std::cout << "gx_commands=" << gx.CommandsProcessed()
              << " gx_frames=" << gx.FramesCopied() << " stalled=" << gx.IsStalled() << "\n";
    const bool waitingInExi = cpu.registers.PC >= 0x80027dc0u && cpu.registers.PC <= 0x80027dc8u;
    std::cout << "exi_wait_completed_naturally=" << (waitingInExi ? "no" : "yes") << "\n";
    if (gx.IsStalled()) std::cout << "GX error: " << gx.LastError() << "\n";
    std::cout << "EXI reads=" << std::dec << exi.MmioReads() << " writes=" << exi.MmioWrites()
              << " start_requests=" << exi.TransferStartRequests() << " completed=" << exi.TransfersCompleted()
              << " | ch0_csr=0x" << std::hex << exi.Status(0)
              << " ch0_cr=0x" << exi.Control(0) << " ch0_mar=0x" << exi.DmaAddress(0)
              << " ch0_len=0x" << exi.DmaLength(0)
              << " ch0_data=0x" << exi.ImmediateData(0) << "\n";
    for (const auto& range : {std::pair{0x80027db4u, 0x80027e18u},
                              std::pair{0x80027e80u, 0x80027fc0u},
                              std::pair{0x80028590u, 0x800285dcu}}) {
        std::cout << "EXI disassembly " << std::hex << range.first << "-";
        for (uint32_t address = range.first; address <= range.second; address += 4) {
            const uint32_t raw = cpu.memory.Read32(address);
            std::cout << "\n  " << std::hex << address << "  0x" << raw << "  "
                      << Broadway::Decoder::Format(Broadway::Decoder::Decode(raw), address);
        }
        std::cout << "\n";
    }
}
void TestStarletArmCp15() {
    Broadway::StarletArm arm(0x4000);
    arm.SetCp15Register(1, 0x1u);
    arm.SetCp15Register(2, 0x1000u);
    arm.memory[0x1000] = 0x12; arm.memory[0x1001] = 0x34; arm.memory[0x1002] = 0x50; arm.memory[0x1003] = 0x02;
    Check(arm.GetCp15Register(1) == 0x1u && arm.mmuEnabled,
          "Starlet CP15 control register enables the MMU when bit 0 is set");
    Check(arm.GetCp15Register(2) == 0x1000u && arm.TTBR0 == 0x1000u,
          "Starlet CP15 translation-table base register retains its value");
    Check(arm.TranslateVirtualAddress(0x00001000u) == 0x12301000u,
          "Starlet virtual-address translation resolves a section descriptor from the first-level page table");
    arm.SetCp15Register(1, 0x0u);
    Check(!arm.mmuEnabled && arm.TranslateVirtualAddress(0x12345678u) == 0x12345678u,
          "Starlet virtual-address translation returns the original address when the MMU is disabled");
}

void TestStarletSparseAddressSpace() {
    Broadway::StarletArm arm;
    Check(arm.memory.size() == size_t(0x100000000ull),
          "Starlet models a 32-bit address space without eagerly allocating it");
    Check(arm.InMemory(0x005C1FA4u) && arm.InMemory(0x10000000u) &&
          arm.InMemory(0x13FFFFFFu) && !arm.InMemory(0x20000000u),
          "Starlet maps documented MEM1 and MEM2 RAM ranges without treating other addresses as RAM");
    Equal(arm.memory.AllocatedPages(), size_t(0),
          "Starlet address space starts with no committed backing pages");
    bool unmappedFault = false;
    try { (void)arm.Read32(0xFFFF0000u); }
    catch (const std::exception&) { unmappedFault = true; }
    Check(unmappedFault, "Unmapped Starlet addresses fault instead of behaving like RAM");
    Equal(arm.memory.AllocatedPages(), size_t(0),
          "A rejected Starlet memory access does not allocate backing pages");
    arm.memory.MapRange(0xFFFF0000u, 0x1000u);
    arm.Write32(0xFFFF0000u, 0xE3A00001u);
    Equal(arm.Read32(0xFFFF0000u), 0xE3A00001u,
          "Starlet sparse memory stores and fetches a high-address ARM instruction");
    Equal(arm.memory.AllocatedPages(), size_t(1),
          "A Starlet write commits only the affected memory page");
    uint32_t mmioAddress = 0;
    arm.mmioRead = [&](uint32_t address) { mmioAddress = address; return 0x12345678u; };
    Equal(arm.Read32(0x0D80018Cu), 0x12345678u,
          "Unmapped Starlet hardware reads are dispatched to the MMIO interface");
    Equal(mmioAddress, 0x0D80018Cu,
          "Starlet MMIO dispatch preserves the guest hardware address");
}

void TestStarletHardwareRegisters() {
    Broadway::StarletHardware hardware;
    Check((hardware.Read(Broadway::StarletHardware::HwBoot0, 4) &
           Broadway::StarletHardware::BootRomDisabled) == 0,
          "Starlet reset exposes the boot ROM mapping");
    const uint32_t boot0 = hardware.Read(Broadway::StarletHardware::HwBoot0, 4);
    hardware.Write(Broadway::StarletHardware::HwBoot0, 4,
                   boot0 | Broadway::StarletHardware::BootRomDisabled);
    Check((hardware.Read(Broadway::StarletHardware::HwBoot0, 4) &
           Broadway::StarletHardware::BootRomDisabled) != 0,
          "HW_BOOT0 bit 12 disables boot ROM visibility and retains its state");
    hardware.Write(Broadway::StarletHardware::HwSrnProt, 4, 0x20u);
    Equal(hardware.Read(Broadway::StarletHardware::HwSrnProt, 4), 0x20u,
          "HW_SRNPROT retains the SRAM mapping control written by IOS");
    bool widthFault = false;
    try { (void)hardware.Read(Broadway::StarletHardware::HwBoot0, 2); }
    catch (const std::exception&) { widthFault = true; }
    Check(widthFault, "Starlet hardware rejects unsupported register access widths");
    bool addressFault = false;
    try { (void)hardware.Read(0x0D800000u, 4); }
    catch (const std::exception&) { addressFault = true; }
    Check(addressFault, "Unknown Starlet registers remain explicit instead of returning fabricated values");
    Broadway::StarletArm arm;
    arm.mmioReadAccess = [&](uint32_t address, unsigned width) {
        return hardware.Read(address, width);
    };
    arm.mmioWriteAccess = [&](uint32_t address, unsigned width, uint32_t value) {
        hardware.Write(address, width, value);
    };
    arm.Write32(Broadway::StarletHardware::HwBoot0, Broadway::StarletHardware::BootRomDisabled);
    Equal(arm.Read32(Broadway::StarletHardware::HwBoot0), Broadway::StarletHardware::BootRomDisabled,
          "Starlet CPU MMIO reads and writes route through the register model with width information");
    bool cpuWidthFault = false;
    try { (void)arm.Read16(Broadway::StarletHardware::HwBoot0); }
    catch (const std::exception&) { cpuWidthFault = true; }
    Check(cpuWidthFault, "Starlet CPU preserves the access width when dispatching hardware reads");
}

void TestStarletArmCore() {
    std::vector<uint8_t> elf(52 + 32 + 4, 0);
    elf[0]=0x7f; elf[1]='E'; elf[2]='L'; elf[3]='F'; elf[4]=1; elf[5]=2;
    PutBE16(elf,16,2); PutBE16(elf,18,40); PutBE32(elf,24,0x100); PutBE32(elf,28,52);
    PutBE16(elf,40,32); PutBE16(elf,44,1); PutBE32(elf,52,1); PutBE32(elf,56,84); PutBE32(elf,60,0x100);
    PutBE32(elf,64,4); PutBE32(elf,68,4); PutBE32(elf,72,4); PutBE32(elf,76,5);
    PutBE32(elf,84,0xE0810002); // add r0,r1,r2
    auto image = Broadway::ArmElfLoader::Parse(elf);
    Check(image.machine == 40 && image.entry == 0x100 && image.segments.size() == 1,
          "ARM ELF loader validates ARM machine and PT_LOAD metadata");
    Broadway::StarletArm arm(0x1000); arm.R[1]=7; arm.R[2]=5; arm.R[15]=0x100; arm.Write32(0x100,0xE0810002); arm.Step();
    Check(arm.R[0] == 12 && arm.R[15] == 0x104 && arm.instructions == 1,
          "Starlet ARM core executes data-processing ADD and advances PC");
    Broadway::StarletArm branchArm(0x1000);
    branchArm.R[15]=0x100; branchArm.R[14]=0xAABBCCDDu;
    branchArm.Write32(0x100,0xEA000000u); // B to PC+8
    branchArm.Step();
    Check(branchArm.R[15]==0x108 && branchArm.R[14]==0xAABBCCDDu,
          "Starlet ARM B branches to PC+8 without clobbering LR");
    branchArm.R[15]=0x100;
    branchArm.Write32(0x100,0xEB000000u); // BL to PC+8
    branchArm.Step();
    Check(branchArm.R[15]==0x108 && branchArm.R[14]==0x104,
          "Starlet ARM BL stores the address of the following instruction in LR");

    Broadway::StarletArm statusArm(0x1000);
    statusArm.SetSPSR(0xA00000D2u);
    statusArm.R[15] = 0x100;
    statusArm.Write32(0x100, 0xE14F0000u); // MRS r0, SPSR
    statusArm.Step();
    Equal(statusArm.R[0], 0xA00000D2u,
          "Starlet ARM MRS reads SPSR rather than CPSR");
    statusArm.R[1] = 0x600000D3u;
    statusArm.R[15] = 0x100;
    statusArm.Write32(0x100, 0xE16FF001u); // MSR SPSR_fsxc, r1
    statusArm.Step();
    Equal(statusArm.SPSR(), 0x600000D3u,
          "Starlet ARM MSR writes selected fields of SPSR");
    statusArm.R[1] = 0x800000D3u;
    statusArm.R[15] = 0x100;
    statusArm.Write32(0x100, 0xE12FF001u); // MSR CPSR_fsxc, r1
    statusArm.Step();
    Equal(statusArm.CPSR, 0x800000D3u,
          "Starlet ARM MSR CPSR remains distinct from MSR SPSR");
}

void TestStarletArmThumbAndExceptions() {
    Broadway::StarletArm arm(0x1000);
    arm.CPSR = 0xD3 | 0x20; // Thumb state, SVC mode
    arm.R[15] = 0x100;
    arm.Write16(0x100, 0x2005); // MOV r0, #5 (Thumb)
    arm.Step();
    Check(arm.R[0] == 5 && arm.R[15] == 0x102 && arm.Thumb(),
          "Starlet Thumb core executes MOV immediate and advances by two");
    {
        Broadway::StarletArm a(0x1000);
        a.CPSR = 0xD3 | 0x20;
        a.R[3] = 0x80;
        a.R[15] = 0x100;
        a.Write16(0x100, 0x0899); // ASR r1, r3, #2
        a.Step();
        Check(a.R[1] == 0x20, "Starlet Thumb core decodes ASR shifted register with correct opcode field");
    }
    // SWI exception entry in ARM state
    Broadway::StarletArm arm2(0x1000);
    arm2.R[15] = 0x100;
    arm2.Write32(0x100, 0xEF000000); // SWI
    arm2.Step();
    Check(arm2.Mode() == 0x13 && arm2.R[14] == 0x104 && arm2.R[15] == 0x08,
          "Starlet ARM SWI enters SVC mode with correct return address and vector");
    // Banked register restore across modes
    Broadway::StarletArm arm3(0x1000);
    arm3.ChangeMode(0x12); // IRQ
    arm3.R[13] = 0xAAAA0000u;
    arm3.ChangeMode(0x13); // SVC
    Check(arm3.R[13] != 0xAAAA0000u, "Banked IRQ stack is not visible in SVC mode");
    arm3.ChangeMode(0x12);
    Check(arm3.R[13] == 0xAAAA0000u, "Banked IRQ stack is restored when re-entering IRQ mode");
}

void TestStarletIosBootContent() {
    // Synthetic IOS boot container: 16-byte header + stub + ELF.
    std::vector<uint8_t> elf(52 + 32 + 4, 0);
    elf[0]=0x7f; elf[1]='E'; elf[2]='L'; elf[3]='F'; elf[4]=1; elf[5]=2;
    PutBE16(elf,16,2); PutBE16(elf,18,40); PutBE32(elf,24,0xffff0000u); PutBE32(elf,28,52);
    PutBE16(elf,40,32); PutBE16(elf,44,1);
    PutBE32(elf,52,1); PutBE32(elf,56,84); PutBE32(elf,60,0xffff0000u);
    PutBE32(elf,64,0xffff0000u); PutBE32(elf,68,4); PutBE32(elf,72,4); PutBE32(elf,76,5);
    PutBE32(elf,84,0xE1A00000u); // MOV r0, r0
    std::vector<uint8_t> container(16 + 4 + elf.size(), 0);
    PutBE32(container,0,0x00000010u);
    PutBE32(container,4,0x00000004u);
    PutBE32(container,8,uint32_t(elf.size()));
    PutBE32(container,12,0);
    container[16] = 0x12;
    container[17] = 0x34;
    container[18] = 0x56;
    container[19] = 0x78;
    std::copy(elf.begin(), elf.end(), container.begin() + 20);
    auto image = Broadway::ArmElfLoader::ParseIosBootContent(container);
    Check(image.entry == 0xffff0000u && image.segments.size() == 1,
          "IOS boot content parser extracts its embedded ELF and validates its entry point");
    Check(image.bootPayload.size() == 4 && image.bootPayload[0] == 0x12 &&
          image.bootPayload[3] == 0x78,
          "IOS boot content parser preserves the genuine pre-ELF payload instead of discarding it");
    Broadway::StarletArm arm(0x100000000ull);
    arm.Load(image);
    Check(arm.R[15] == 0xffff0000u && arm.Read32(0xffff0000u) == 0xE1A00000u,
          "IOS boot content ELF is loaded at its declared high memory address");
    arm.Step();
    Check(arm.R[15] == 0xffff0004u && arm.InstructionCount() == 1,
          "IOS boot content executes a verified instruction from high memory");
}

void TestStarletArmElfLoading() {
    std::vector<uint8_t> elf(52 + 32 + 8, 0);
    elf[0]=0x7f; elf[1]='E'; elf[2]='L'; elf[3]='F'; elf[4]=1; elf[5]=2;
    PutBE16(elf,16,2); PutBE16(elf,18,40); PutBE32(elf,24,0x100); PutBE32(elf,28,52);
    PutBE16(elf,40,32); PutBE16(elf,44,1);
    PutBE32(elf,52,1); PutBE32(elf,56,84); PutBE32(elf,60,0x100);
    PutBE32(elf,64,0x100); PutBE32(elf,68,4); PutBE32(elf,72,8); PutBE32(elf,76,5);
    PutBE32(elf,84,0xE0810002);
    auto image = Broadway::ArmElfLoader::Parse(elf);
    Check(image.segments.size() == 1 && image.segments[0].fileSize == 4 && image.segments[0].memorySize == 8,
          "ARM ELF parser retains file and memory sizes for BSS handling");
    Broadway::StarletArm arm(0x1000);
    arm.Load(image);
    Check(arm.R[15] == 0x100, "ARM ELF loader sets PC to entry point");
    Check(arm.Read32(0x100) == 0xE0810002, "ARM ELF loader copies PT_LOAD file bytes");
    Check(arm.Read32(0x104) == 0, "ARM ELF loader zeroes BSS tail");

    PutBE32(elf,24,0x20000000u);
    PutBE32(elf,60,0x20000000u);
    PutBE32(elf,64,0x10001000u);
    auto physicalImage = Broadway::ArmElfLoader::Parse(elf);
    Broadway::StarletArm physicalArm;
    physicalArm.Load(physicalImage);
    Check(physicalArm.Read32(0x10001000u) == 0xE0810002u &&
          physicalImage.segments[0].address == 0x20000000u &&
          physicalImage.segments[0].physicalAddress == 0x10001000u,
          "Starlet ELF loading places PT_LOAD bytes at p_paddr while preserving the virtual address");
    bool virtualUnmapped = false;
    try { (void)physicalArm.Read32(0x20000000u); }
    catch (const std::exception&) { virtualUnmapped = true; }
    Check(virtualUnmapped, "Starlet does not silently treat an ELF virtual address as physical RAM");
}

int main() {
    try {
        TestMemoryAndInteger();
        TestEXIControllerRegisters();
        TestMSRInstructions();
        TestSegmentRegisterInstructions();
        TestTimeBaseInstructions();
        TestHardwareRegisterInstructions();
        TestBranchesAndDebugger();
        TestLoadsStoresAndExceptions();
        TestIndexedFloatingPointMemory();
        TestMtfsb1();
        TestCacheInstructions();
        TestPairedSingle();
        TestDecoder();
        TestVideoFrames();
        TestGXDirectQuadRasterization();
        TestGXCommandIngressAndCopy();
        TestExecutableLoading();
        TestWiiDiscImageReader();
        TestSystemCallExceptionEntry();
        TestWiiBootLowMemory();
        TestWiiFirmwareInventory();
        TestWiiSystemEnvironmentState();
        TestStarletArmCp15();
        TestStarletSparseAddressSpace();
        TestStarletHardwareRegisters();
        TestStarletArmCore();
        TestHomebrewDol();
        TestStarletArmThumbAndExceptions();
        TestStarletArmElfLoading();
        TestStarletIosBootContent();
        std::cout << "BroadwayCPU core tests passed: " << g_checks << " checks\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "BroadwayCPU core test failed after " << g_checks << " checks: " << e.what() << "\n";
        return 1;
    }
}
