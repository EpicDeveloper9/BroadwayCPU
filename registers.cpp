#include "registers.hpp"
namespace Broadway {
void CPURegisters::Reset() {
    GPR.fill(0);
    FPR.fill(0.0);
    PC = LR = CTR = XER = CR = MSR = FPSCR = 0;
}
}
