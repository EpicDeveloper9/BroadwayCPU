#include "exceptions.hpp"
#include <sstream>
namespace Broadway {
CPUException::CPUException(ExceptionKind k, uint32_t faultPC, const std::string& message)
    : std::runtime_error([&] {
        std::ostringstream s;
        s << message << " (PC=0x" << std::hex << faultPC << ")";
        return s.str();
      }()), kind(k), pc(faultPC) {}
}
