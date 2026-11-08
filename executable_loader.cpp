#include "executable_loader.hpp"

#include "cpu.hpp"

#include <limits>
#include <stdexcept>

namespace Broadway {
namespace {
struct Segment {
    uint32_t address;
    size_t fileOffset;
    size_t fileSize;
    size_t memorySize;
    bool executable;
};

uint16_t ReadBE16(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2)
        throw std::runtime_error("Executable header is truncated.");
    return uint16_t((uint16_t(bytes[offset]) << 8) | bytes[offset + 1]);
}

uint32_t ReadBE32(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("Executable header is truncated.");
    return (uint32_t(bytes[offset]) << 24) | (uint32_t(bytes[offset + 1]) << 16) |
        (uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

void ValidateFileRange(const std::vector<uint8_t>& bytes, uint64_t offset, uint64_t size) {
    if (offset > bytes.size() || size > bytes.size() - size_t(offset))
        throw std::runtime_error("Executable segment extends past the end of the file.");
}

ExecutableLoadResult ParseDol(CPU& cpu, const std::vector<uint8_t>& bytes,
                              std::vector<Segment>& segments, std::vector<Segment>& zeroFill) {
    if (bytes.size() < 0x100) throw std::runtime_error("DOL header is incomplete.");
    bool hasText = false;
    for (unsigned section = 0; section < 18; ++section) {
        const uint32_t fileOffset = ReadBE32(bytes, section * 4u);
        const uint32_t address = ReadBE32(bytes, 0x48u + section * 4u);
        const uint32_t size = ReadBE32(bytes, 0x90u + section * 4u);
        if (size == 0) continue;
        ValidateFileRange(bytes, fileOffset, size);
        cpu.memory.Validate(address, size);
        segments.push_back({address, fileOffset, size, size, section < 7});
        hasText = hasText || section < 7;
    }
    if (segments.empty()) throw std::runtime_error("No loadable DOL sections were found.");
    if (!hasText) throw std::runtime_error("DOL contains no text section.");

    const uint32_t bssAddress = ReadBE32(bytes, 0xD8);
    const uint32_t bssSize = ReadBE32(bytes, 0xDC);
    if (bssSize) {
        cpu.memory.Validate(bssAddress, bssSize);
        zeroFill.push_back({bssAddress, 0, 0, bssSize, false});
    }

    const uint32_t entry = ReadBE32(bytes, 0xE0);
    if (entry & 3u) throw std::runtime_error("DOL entry point is not word aligned.");
    bool entryInText = false;
    for (const auto& segment : segments) {
        if (segment.executable && entry >= segment.address &&
            uint64_t(entry) + 4u <= uint64_t(segment.address) + segment.memorySize) {
            entryInText = true;
            break;
        }
    }
    if (!entryInText) throw std::runtime_error("DOL entry point is outside its text sections.");
    return {entry, segments.size(), "DOL"};
}

ExecutableLoadResult ParseElf(CPU& cpu, const std::vector<uint8_t>& bytes,
                              std::vector<Segment>& segments) {
    if (bytes.size() < 52 || bytes[0] != 0x7F || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F')
        throw std::runtime_error("Not a valid ELF file.");
    if (bytes[4] != 1 || bytes[5] != 2 || bytes[6] != 1)
        throw std::runtime_error("Only ELF32, big-endian, version 1 executables are supported.");
    if (ReadBE16(bytes, 16) != 2 || ReadBE16(bytes, 18) != 20)
        throw std::runtime_error("ELF must be an executable for 32-bit PowerPC.");
    if (ReadBE32(bytes, 20) != 1 || ReadBE16(bytes, 40) < 52)
        throw std::runtime_error("ELF header version or size is invalid.");

    const uint32_t entry = ReadBE32(bytes, 24);
    const uint32_t programOffset = ReadBE32(bytes, 28);
    const uint16_t programEntrySize = ReadBE16(bytes, 42);
    const uint16_t programCount = ReadBE16(bytes, 44);
    if (programEntrySize < 32) throw std::runtime_error("Invalid ELF program-header size.");
    ValidateFileRange(bytes, programOffset, uint64_t(programEntrySize) * programCount);

    for (uint16_t index = 0; index < programCount; ++index) {
        const size_t p = size_t(programOffset) + size_t(index) * programEntrySize;
        if (ReadBE32(bytes, p) != 1) continue;
        const uint32_t offset = ReadBE32(bytes, p + 4);
        const uint32_t virtualAddress = ReadBE32(bytes, p + 8);
        const uint32_t fileSize = ReadBE32(bytes, p + 16);
        const uint32_t memorySize = ReadBE32(bytes, p + 20);
        const uint32_t flags = ReadBE32(bytes, p + 24);
        if (fileSize > memorySize) throw std::runtime_error("ELF segment file size exceeds its memory size.");
        if (memorySize == 0) continue;
        ValidateFileRange(bytes, offset, fileSize);
        cpu.memory.Validate(virtualAddress, memorySize);
        segments.push_back({virtualAddress, offset, fileSize, memorySize, (flags & 1u) != 0});
    }
    if (segments.empty()) throw std::runtime_error("ELF contains no loadable segments.");
    if (entry & 3u) throw std::runtime_error("ELF entry point is not word aligned.");
    bool entryMapped = false;
    for (const auto& segment : segments) {
        if (segment.executable && entry >= segment.address &&
            uint64_t(entry) + 4u <= uint64_t(segment.address) + segment.memorySize) {
            entryMapped = true;
            break;
        }
    }
    if (!entryMapped) throw std::runtime_error("ELF entry point is outside executable segments.");
    return {entry, segments.size(), "ELF32 PowerPC"};
}

void Commit(CPU& cpu, const std::vector<uint8_t>& bytes, const std::vector<Segment>& segments,
            const std::vector<Segment>& zeroFill) {
    cpu.memory.Clear();
    cpu.Reset();
    for (const auto& segment : segments) {
        for (size_t n = 0; n < segment.fileSize; ++n)
            cpu.memory.Write8(segment.address + uint32_t(n), bytes[segment.fileOffset + n]);
        for (size_t n = segment.fileSize; n < segment.memorySize; ++n)
            cpu.memory.Write8(segment.address + uint32_t(n), 0);
    }
    for (const auto& segment : zeroFill)
        for (size_t n = 0; n < segment.memorySize; ++n)
            cpu.memory.Write8(segment.address + uint32_t(n), 0);
}
}

ExecutableLoadResult ExecutableLoader::Load(CPU& cpu, const std::vector<uint8_t>& file,
                                             ExecutableFormat format, uint32_t rawLoadAddress) {
    if (file.empty()) throw std::runtime_error("The selected executable is empty.");
    if (format == ExecutableFormat::Auto) {
        format = file.size() >= 4 && file[0] == 0x7F && file[1] == 'E' && file[2] == 'L' && file[3] == 'F'
            ? ExecutableFormat::Elf : ExecutableFormat::Raw;
    }

    std::vector<Segment> segments;
    std::vector<Segment> zeroFill;
    ExecutableLoadResult result;
    if (format == ExecutableFormat::Dol) {
        result = ParseDol(cpu, file, segments, zeroFill);
    } else if (format == ExecutableFormat::Elf) {
        result = ParseElf(cpu, file, segments);
    } else if (format == ExecutableFormat::Raw) {
        if (file.size() > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("Raw binary is too large for the 32-bit address space.");
        if (rawLoadAddress & 3u) throw std::runtime_error("Raw binary load address must be word aligned.");
        cpu.memory.Validate(rawLoadAddress, file.size());
        segments.push_back({rawLoadAddress, 0, file.size(), file.size(), true});
        result = {rawLoadAddress, 1, "Raw binary"};
    } else {
        throw std::runtime_error("Unsupported executable format.");
    }

    Commit(cpu, file, segments, zeroFill);
    cpu.registers.PC = result.entryPoint;
    return result;
}
}
