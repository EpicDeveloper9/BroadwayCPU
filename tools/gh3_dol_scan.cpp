#include "../decoder.hpp"

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace {
uint32_t BE32(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 4 > data.size()) throw std::runtime_error("Truncated DOL header");
    return (uint32_t(data[offset]) << 24) | (uint32_t(data[offset + 1]) << 16) |
        (uint32_t(data[offset + 2]) << 8) | data[offset + 3];
}

std::string Csv(const std::string& value) {
    std::string result = "\"";
    for (char c : value) { if (c == '"') result += '"'; result += c; }
    result += '"';
    return result;
}

bool HasSemanticTest(const std::string& mnemonic) {
    return mnemonic == "addi" || mnemonic == "addic" || mnemonic == "and" ||
        mnemonic == "adde" || mnemonic == "extsh" || mnemonic == "cmpwi" || mnemonic == "b" ||
        mnemonic == "stw" || mnemonic == "lwz" || mnemonic == "mfmsr" ||
        mnemonic == "mtmsr" || mnemonic == "rfi" || mnemonic == "isync" ||
        mnemonic == "lfsx" || mnemonic == "stfsx" || mnemonic == "lfdx" ||
        mnemonic == "sthbrx" || mnemonic == "lwbrx" || mnemonic == "mtsr" ||
        mnemonic == "mfsr" || mnemonic == "mtsrin" || mnemonic == "mfsrin" ||
        mnemonic == "mftb" || mnemonic == "ps_sum0" ||
        mnemonic == "psq_l" || mnemonic == "psq_st" || mnemonic == "mtfsb1";
}

bool HasImplementedSemantics(const Broadway::Instruction& i, const std::string& mnemonic) {
    if (!i.implemented || !i.dispatchable) return false;
    if (mnemonic == "dcbst" || mnemonic == "dcbf" || mnemonic == "dcbt" ||
        mnemonic == "dcbtst" || mnemonic == "icbi") return false;
    // sc currently reports a host-side event; architectural exception entry
    // (vector, SRR capture and MSR transition) is not yet modeled.
    if (i.opcode == 17) return false;
    if (mnemonic == "mfspr" || mnemonic == "mtspr") {
        const unsigned spr = ((i.raw >> 16) & 31u) | ((i.raw >> 6) & 0x3E0u);
        if (mnemonic == "mfspr") return spr == 1 || spr == 8 || spr == 9 || spr == 26 || spr == 27 ||
            spr == 18 || spr == 19 || spr == 22 || spr == 25 || spr == 1008 || spr == 1009 ||
            (spr >= 272 && spr <= 275) || (spr >= 528 && spr <= 543) ||
            (spr >= 912 && spr <= 920) || spr == 921 || spr == 1017;
        return spr == 1 || spr == 8 || spr == 9 || spr == 18 || spr == 19 || spr == 22 || spr == 25 ||
            spr == 26 || spr == 27 || spr == 284 || spr == 285 || spr == 1008 || spr == 1009 ||
            (spr >= 272 && spr <= 275) || (spr >= 528 && spr <= 543) ||
            (spr >= 912 && spr <= 920) || spr == 921 || spr == 1017;
    }
    if (mnemonic == "mftb") {
        const unsigned tbr = ((i.raw >> 16) & 31u) | ((i.raw >> 6) & 0x3E0u);
        return tbr == 268 || tbr == 269;
    }
    return true;
}
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: gh3_dol_scan <DATA/sys/main.dol> <output.csv>\n";
        return 2;
    }
    try {
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("Could not open DOL");
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(input)), {});
        if (file.size() < 0x100) throw std::runtime_error("DOL header is incomplete");

        std::map<uint32_t, uint64_t> encodings;
        std::vector<std::pair<uint64_t,uint64_t>> fileRanges;
        uint64_t instructionCount = 0;
        size_t textBytes = 0;
        for (unsigned section = 0; section < 7; ++section) {
            const uint32_t offset = BE32(file, section * 4u);
            const uint32_t address = BE32(file, 0x48u + section * 4u);
            const uint32_t size = BE32(file, 0x90u + section * 4u);
            if (size == 0) continue;
            if (offset < 0x100u || (offset & 3u) != 0 || (address & 3u) != 0)
                throw std::runtime_error("Text section has an invalid offset or address alignment");
            if (uint64_t(address) + uint64_t(size) > 0x100000000ull)
                throw std::runtime_error("Text section address range wraps the 32-bit address space");
            if (uint64_t(offset) + uint64_t(size) > uint64_t(file.size()))
                throw std::runtime_error("Text section extends beyond file");
            if ((size & 3u) != 0) throw std::runtime_error("Text section is not word sized");
            for (const auto& range : fileRanges)
                if (uint64_t(offset) < range.second && range.first < uint64_t(offset) + size)
                    throw std::runtime_error("Text sections overlap in the DOL file");
            fileRanges.emplace_back(offset, uint64_t(offset) + size);
            textBytes += size;
            for (uint32_t pos = 0; pos < size; pos += 4) {
                const size_t p = size_t(offset) + pos;
                const uint32_t raw = (uint32_t(file[p]) << 24) | (uint32_t(file[p + 1]) << 16) |
                    (uint32_t(file[p + 2]) << 8) | file[p + 3];
                ++encodings[raw];
                ++instructionCount;
            }
        }

        std::ofstream csv(argv[2], std::ios::binary);
        if (!csv) throw std::runtime_error("Could not create CSV report");
        csv << "raw_encoding,mnemonic,frequency,decoded,valid_encoding,has_dispatch_path,implemented_flag,implemented_semantics,semantically_tested,status\n";
        std::map<std::string, uint64_t> mnemonicFrequency;
        using Family = std::tuple<unsigned,unsigned,std::string,std::string,bool,bool,bool,bool,bool>;
        std::map<Family,uint64_t> families;
        std::map<Family,uint32_t> familyRepresentatives;
        uint64_t unsupported = 0, unrecognized = 0, validOccurrences = 0, invalidOccurrences = 0;
        uint64_t supportedOccurrences = 0, dispatchOccurrences = 0, testedOccurrences = 0;
        for (const auto& [raw, frequency] : encodings) {
            const auto instruction = Broadway::Decoder::Decode(raw);
            const std::string mnemonic = Broadway::Decoder::Name(instruction);
            const bool semantics = HasImplementedSemantics(instruction, mnemonic);
            const std::string validity = !instruction.validityKnown ? "unclassified" :
                (instruction.validEncoding ? "yes" : "no");
            mnemonicFrequency[mnemonic] += frequency;
            if (!instruction.recognized) unrecognized += frequency;
            if (instruction.validityKnown && instruction.validEncoding) validOccurrences += frequency;
            if (instruction.validityKnown && !instruction.validEncoding) invalidOccurrences += frequency;
            if (instruction.implemented) supportedOccurrences += frequency;
            if (instruction.dispatchable) dispatchOccurrences += frequency;
            if (HasSemanticTest(mnemonic)) testedOccurrences += frequency;
            if (!semantics) unsupported += frequency;
            const char* status = semantics ? "semantics-implemented" :
                (instruction.recognized ? "recognized-but-not-implemented" :
                 (validity == "no" ? "invalid-encoding" : "unrecognized-or-unclassified"));
            const bool tested = HasSemanticTest(mnemonic);
            csv << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << raw
                << ',' << Csv(mnemonic) << ',' << std::dec << frequency << ','
                << (instruction.recognized ? "yes" : "no") << ','
                << validity << ','
                << (instruction.dispatchable ? "yes" : "no") << ','
                << (instruction.implemented ? "yes" : "no") << ','
                << (semantics ? "yes" : "no") << ','
                << (tested ? "family-test" : "no") << ',' << status << '\n';
            const unsigned primary = raw >> 26;
            const unsigned extended = primary == 31 ? ((raw >> 1) & 0x3FFu) :
                (primary == 19 || primary == 4 ? ((raw >> 1) & 0x3FFu) : ((raw >> 21) & 31u));
            const Family family{primary, extended, mnemonic, validity, instruction.recognized,
                instruction.dispatchable, instruction.implemented, semantics, tested};
            families[family] += frequency;
            familyRepresentatives.try_emplace(family, raw);
        }
        std::string familyPath = argv[2];
        familyPath += ".families.csv";
        std::ofstream familyCsv(familyPath, std::ios::binary);
        if (!familyCsv) throw std::runtime_error("Could not create family report");
        familyCsv << "primary_opcode,extended_or_operand_field,mnemonic,occurrences,representative_encoding,decoded,valid_encoding,has_dispatch_path,implemented_flag,implemented_semantics,semantically_tested\n";
        for (const auto& [family, frequency] : families) {
            const auto& [primary, extended, mnemonic, validity, decoded, dispatch, implemented, semantics, tested] = family;
            familyCsv << primary << ',' << extended << ',' << Csv(mnemonic) << ',' << frequency
                << ",0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << familyRepresentatives[family]
                << std::dec << ',' << (decoded ? "yes" : "no") << ',' << validity << ',' << (dispatch ? "yes" : "no")
                << ',' << (implemented ? "yes" : "no") << ',' << (semantics ? "yes" : "no")
                << ',' << (tested ? "family-test" : "no") << '\n';
        }
        std::cout << "DOL bytes: " << file.size() << "\nText bytes scanned: " << textBytes
            << "\nInstructions scanned: " << instructionCount << "\nUnique raw encodings: " << encodings.size()
            << "\nUnique decoder labels: " << mnemonicFrequency.size()
            << "\nOccurrences validated as encodings: " << validOccurrences
            << "\nOccurrences classified as invalid: " << invalidOccurrences
            << "\nOccurrences with unknown validity: " << instructionCount - validOccurrences - invalidOccurrences
            << "\nOccurrences with a dispatch-class path: " << dispatchOccurrences
            << "\nOccurrences with implemented flag: " << supportedOccurrences
            << "\nOccurrences with audited implemented semantics: " << (instructionCount - unsupported)
            << "\nOccurrences without audited implemented semantics: " << unsupported
            << "\nOccurrences in a semantically tested mnemonic family: " << testedOccurrences
            << "\nUnrecognized occurrences: " << unrecognized
            << "\nCSV: " << argv[2] << "\nFamily CSV: " << familyPath
            << "\nCaution: text sections may contain embedded data; this scan is not runtime evidence.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "DOL scan failed: " << e.what() << '\n';
        return 1;
    }
}
