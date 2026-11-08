#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Broadway {
class CPU;

enum class ExecutableFormat : uint8_t { Auto, Dol, Elf, Raw };

struct ExecutableLoadResult {
    uint32_t entryPoint = 0;
    size_t segmentCount = 0;
    std::string formatName;
};

class ExecutableLoader {
public:
    static ExecutableLoadResult Load(CPU& cpu, const std::vector<uint8_t>& file,
                                     ExecutableFormat format = ExecutableFormat::Auto,
                                     uint32_t rawLoadAddress = 0x80004000u);
};
}
