#include "registers.hpp"
namespace Broadway {
void CPURegisters::Reset() {
    GPR.fill(0);
    FPR.fill(0.0);
    PC = LR = CTR = XER = CR = MSR = FPSCR = HID0 = HID1 = HID2 = HID4 = WPAR = L2CR = MMCR0 = MMCR1 = DAR = DSISR = SDR1 = DEC = SRR0 = SRR1 = 0;
    GQR.fill(0);
    PMC.fill(0);
    SR.fill(0);
    SPRG.fill(0);
    for (auto& bat : IBAT) bat.fill(0);
    for (auto& bat : DBAT) bat.fill(0);
    TimeBase = 0;
}
}
