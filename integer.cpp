#include "cpu.hpp"
#include <bit>
#include <cstdint>
#include <limits>

namespace Broadway {
namespace {
uint32_t PPCMask(unsigned mb, unsigned me) {
    uint32_t mask = 0;
    for (unsigned bit = 0; bit < 32; ++bit) {
        const bool in = (mb <= me) ? (bit >= mb && bit <= me)
                                   : (bit >= mb || bit <= me);
        if (in) mask |= (1u << (31u - bit));
    }
    return mask;
}
uint32_t SignExtend16(uint32_t v) { return uint32_t(int32_t(int16_t(v))); }
bool HasOEForm(uint32_t xo) {
    switch (xo) {
    case 8: case 10: case 40: case 104: case 136: case 138: case 200: case 202:
    case 232: case 234: case 235: case 266: case 459: case 491: return true;
    default: return false;
    }
}
}

void CPU::ExecuteInteger(const Instruction& ins) {
    const uint32_t raw=ins.raw, op=ins.opcode, rt=ins.rt, ra=ins.ra, rb=ins.rb;
    const uint32_t a=registers.GPR[ra], b=registers.GPR[rb];
    const int32_t simm=int16_t(raw & 0xFFFF);
    const uint32_t uimm=raw & 0xFFFF;
    auto& g=registers.GPR;

    if (op==10 || op==11) {
        const bool logical = op==10;
        const uint32_t bf=(raw>>23)&7;
        const uint32_t lhs=a;
        const uint32_t rhs=logical ? uimm : SignExtend16(uimm);
        uint32_t cr=2;
        if (logical) { if(lhs<rhs) cr=8; else if(lhs>rhs) cr=4; }
        else {
            const int32_t sl=int32_t(lhs), sr=int32_t(rhs);
            if(sl<sr) cr=8; else if(sl>sr) cr=4;
        }
        if(registers.GetXERSummaryOverflow()) cr|=1;
        registers.SetCRField(bf,cr);
        return;
    }

    if(op==19) {
        const uint32_t crxo=(raw>>1)&0x3FFu;
        if(crxo==33 || crxo==129 || crxo==193 || crxo==225 || crxo==257 || crxo==289 || crxo==417 || crxo==449) {
            const unsigned bt=(raw>>21)&31u, ba=(raw>>16)&31u, bb=(raw>>11)&31u;
            const bool av=((registers.CR>>(31u-ba))&1u)!=0;
            const bool bv=((registers.CR>>(31u-bb))&1u)!=0;
            bool out=false;
            switch(crxo) {
            case 33: out=!(av||bv); break; case 129: out=av&&!bv; break;
            case 193: out=av!=bv; break; case 225: out=!(av&&bv); break;
            case 257: out=av&&bv; break; case 289: out=av==bv; break;
            case 417: out=av||!bv; break; case 449: out=av||bv; break;
            }
            const uint32_t mask=1u<<(31u-bt);
            registers.CR=out?(registers.CR|mask):(registers.CR&~mask);
            return;
        }
    }

    switch(op) {
    case 7: g[rt]=uint32_t(int64_t(int32_t(a))*int64_t(simm)); return; // mulli
    case 8: { // subfic: RT = SIMM - RA, CA means no borrow
        const uint64_t wide=uint64_t(uint32_t(~a))+uint32_t(simm)+1ull;
        g[rt]=uint32_t(wide); registers.SetXERCarry((wide>>32)!=0); return;
    }
    case 12: case 13: { // addic[.]
        const uint64_t wide=uint64_t(a)+uint32_t(simm);
        g[rt]=uint32_t(wide); registers.SetXERCarry((wide>>32)!=0);
        if(op==13) UpdateCR0(g[rt]);
        return;
    }
    case 14: g[rt]=(ra ? a : 0u)+uint32_t(simm); return; // addi
    case 15: g[rt]=(ra ? a : 0u)+(uint32_t(simm)<<16); return; // addis
    case 20: case 21: case 23: { // rotate/mask
        const unsigned sh=(op==23) ? (b&31u) : ((raw>>11)&31u);
        const unsigned mb=(raw>>6)&31u, me=(raw>>1)&31u;
        const uint32_t rotated=std::rotl(g[rt],int(sh));
        const uint32_t mask=PPCMask(mb,me);
        if(op==20) g[ra]=(g[ra]&~mask)|(rotated&mask);
        else g[ra]=rotated&mask;
        if(ins.record) UpdateCR0(g[ra]);
        return;
    }
    case 24: g[ra]=g[rt]|uimm; return;
    case 25: g[ra]=g[rt]|(uimm<<16); return;
    case 26: g[ra]=g[rt]^uimm; return;
    case 27: g[ra]=g[rt]^(uimm<<16); return;
    case 28: g[ra]=g[rt]&uimm; UpdateCR0(g[ra]); return;
    case 29: g[ra]=g[rt]&(uimm<<16); UpdateCR0(g[ra]); return;
    case 31: break;
    default: throw CPUException(ExceptionKind::UnimplementedInstruction,lastInstructionPC,"Integer instruction not implemented");
    }

    const uint32_t fullXO=(raw>>1)&0x3FFu;
    const uint32_t lowXO=fullXO&0x1FFu;
    const uint32_t xo=HasOEForm(lowXO)?lowXO:fullXO;
    if (xo==0 || xo==32) { // cmp / cmpl
        const unsigned bf=(raw>>23)&7u;
        uint32_t cr=2;
        if (xo==0) {
            if (int32_t(a)<int32_t(b)) cr=8;
            else if (int32_t(a)>int32_t(b)) cr=4;
        } else {
            if (a<b) cr=8;
            else if (a>b) cr=4;
        }
        if (registers.GetXERSummaryOverflow()) cr|=1;
        registers.SetCRField(bf,cr);
        return;
    }
    if (xo==19) { g[rt]=registers.CR; return; } // mfcr
    if (xo==83) { // mfmsr is privileged on Broadway.
        if (registers.MSR & CPURegisters::MSR_PR)
            throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged mfmsr executed in user mode");
        g[rt]=registers.MSR;
        return;
    }
    if (xo==146) {
        if (registers.MSR & CPURegisters::MSR_PR)
            throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged mtmsr executed in user mode");
        registers.MSR = g[rt];
        return;
    }
    if (xo==144) { // mtcrf
        const uint32_t fxm=(raw>>12)&0xFFu;
        for(unsigned field=0;field<8;++field)
            if(fxm&(1u<<(7u-field)))
                registers.SetCRField(field,(g[rt]>>(28u-field*4u))&0xFu);
        return;
    }
    // CR boolean instructions use CR bit numbering (bit 0 is the MSB).
    if (xo==33 || xo==129 || xo==193 || xo==225 || xo==257 || xo==289 || xo==417 || xo==449) {
        const unsigned bt=(raw>>21)&31u, ba=(raw>>16)&31u, bb=(raw>>11)&31u;
        const bool av=((registers.CR>>(31u-ba))&1u)!=0;
        const bool bv=((registers.CR>>(31u-bb))&1u)!=0;
        bool out=false;
        switch(xo) {
        case 33: out=!(av||bv); break;       // crnor
        case 129: out=av&&!bv; break;        // crandc
        case 193: out=av!=bv; break;         // crxor
        case 225: out=!(av&&bv); break;      // crnand
        case 257: out=av&&bv; break;         // crand
        case 289: out=av==bv; break;         // creqv
        case 417: out=av||!bv; break;        // crorc
        case 449: out=av||bv; break;         // cror
        }
        const uint32_t mask=1u<<(31u-bt);
        registers.CR=out?(registers.CR|mask):(registers.CR&~mask);
        return;
    }

    // Indexed integer loads/stores and reservation primitives.
    const uint32_t ea=a+b;
    auto checkAlign=[&](uint32_t address,unsigned n) {
        if(address&(n-1u)) throw CPUException(ExceptionKind::Alignment,lastInstructionPC,"Unaligned indexed memory access");
    };
    auto load8=[&](uint32_t address)->uint8_t { try { return memory.Read8(address); } catch(...) { throw CPUException(ExceptionKind::DataStorage,lastInstructionPC,"Indexed byte load outside mapped memory"); } };
    auto load16=[&](uint32_t address)->uint16_t { checkAlign(address,2); try { return memory.Read16(address); } catch(...) { throw CPUException(ExceptionKind::DataStorage,lastInstructionPC,"Indexed halfword load outside mapped memory"); } };
    auto load32=[&](uint32_t address)->uint32_t { checkAlign(address,4); try { return memory.Read32(address); } catch(...) { throw CPUException(ExceptionKind::DataStorage,lastInstructionPC,"Indexed word load outside mapped memory"); } };
    auto store8=[&](uint32_t address,uint8_t value) { try { memory.Write8(address,value); } catch(...) { throw CPUException(ExceptionKind::DataStorage,lastInstructionPC,"Indexed byte store outside mapped memory"); } };
    auto store16=[&](uint32_t address,uint16_t value) { checkAlign(address,2); try { memory.Write16(address,value); } catch(...) { throw CPUException(ExceptionKind::DataStorage,lastInstructionPC,"Indexed halfword store outside mapped memory"); } };
    auto store32=[&](uint32_t address,uint32_t value) { checkAlign(address,4); try { memory.Write32(address,value); } catch(...) { throw CPUException(ExceptionKind::DataStorage,lastInstructionPC,"Indexed word store outside mapped memory"); } };
    switch(xo) {
    case 23: g[rt]=load32(ea); return;                         // lwzx
    case 55: if(!ra) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"lwzux requires RA"); g[rt]=load32(ea); g[ra]=ea; return;
    case 87: g[rt]=load8(ea); return;                          // lbzx
    case 119: if(!ra) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"lbzux requires RA"); g[rt]=load8(ea); g[ra]=ea; return;
    case 279: g[rt]=load16(ea); return;                        // lhzx
    case 311: if(!ra) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"lhzux requires RA"); g[rt]=load16(ea); g[ra]=ea; return;
    case 343: g[rt]=uint32_t(int32_t(int16_t(load16(ea)))); return; // lhax
    case 375: if(!ra) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"lhaux requires RA"); g[rt]=uint32_t(int32_t(int16_t(load16(ea)))); g[ra]=ea; return;
    case 151: store32(ea,g[rt]); return;                       // stwx
    case 183: if(!ra) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"stwux requires RA"); store32(ea,g[rt]); g[ra]=ea; return;
    case 215: store8(ea,uint8_t(g[rt])); return;                // stbx
    case 247: if(!ra) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"stbux requires RA"); store8(ea,uint8_t(g[rt])); g[ra]=ea; return;
    case 407: store16(ea,uint16_t(g[rt])); return;              // sthx
    case 439: if(!ra) throw CPUException(ExceptionKind::InvalidInstruction,lastInstructionPC,"sthux requires RA"); store16(ea,uint16_t(g[rt])); g[ra]=ea; return;
    case 533: case 597: {                                      // lswx / lswi
        unsigned count=(xo==597)?((raw>>11)&31u): (registers.XER&0x7Fu);
        if(count==0) count=32;
        uint32_t address=(xo==597)?(ra?g[ra]:0u):ea;
        unsigned reg=rt;
        for(unsigned n=0;n<count;++n) {
            if((n&3u)==0) g[reg]=0;
            const unsigned shift=24u-8u*(n&3u);
            g[reg]|=uint32_t(load8(address+n))<<shift;
            if((n&3u)==3u || n+1u==count) { ++reg; if(reg==32) reg=0; }
        }
        return;
    }
    case 661: case 725: {                                      // stswx / stswi
        unsigned count=(xo==725)?((raw>>11)&31u):(registers.XER&0x7Fu);
        if(count==0) count=32;
        uint32_t address=(xo==725)?(ra?g[ra]:0u):ea;
        unsigned reg=rt;
        for(unsigned n=0;n<count;++n) { const unsigned shift=24u-8u*(n&3u); store8(address+n,uint8_t(g[reg]>>shift)); if((n&3u)==3u) { ++reg; if(reg==32) reg=0; } }
        return;
    }
    default: break;
    }

    const bool oe=ins.oe;
    const bool rc=(raw&1u)!=0;
    const bool oldCA=registers.GetXERCarry();
    uint32_t result=0;
    bool supported=true, updateCarry=false, carry=false;
    bool arithmetic=false;
    int64_t signedWide=0;

    switch(xo) {
    case 210: { // mtsr SR,rS
        if (registers.MSR & CPURegisters::MSR_PR)
            throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged mtsr executed in user mode");
        registers.SR[ra & 0xFu] = g[rt];
        return;
    }
    case 242: { // mtsrin rS,rB; segment index comes from EA[0:3]
        if (registers.MSR & CPURegisters::MSR_PR)
            throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged mtsrin executed in user mode");
        registers.SR[g[rb] >> 28] = g[rt];
        return;
    }
    case 595: // mfsr rD,SR
        if (registers.MSR & CPURegisters::MSR_PR)
            throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged mfsr executed in user mode");
        result = registers.SR[ra & 0xFu];
        break;
    case 659: // mfsrin rD,rB
        if (registers.MSR & CPURegisters::MSR_PR)
            throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged mfsrin executed in user mode");
        result = registers.SR[g[rb] >> 28];
        break;
    case 371: { // mftb: Gekko exposes TBL (268) and TBU (269) to user mode.
        const uint32_t tbr = ((raw >> 16) & 31u) | ((raw >> 6) & 0x3E0u);
        if (tbr == 268) result = uint32_t(registers.TimeBase);
        else if (tbr == 269) result = uint32_t(registers.TimeBase >> 32);
        else throw CPUException(ExceptionKind::Program,lastInstructionPC,"Unsupported TBR selected by mftb");
        break;
    }
    case 8: { uint64_t w=uint64_t(uint32_t(~a))+b+1ull; result=uint32_t(w); updateCarry=true; carry=(w>>32)!=0; arithmetic=true; signedWide=int64_t(int32_t(b))-int64_t(int32_t(a)); break; } // subfc
    case 10: { uint64_t w=uint64_t(a)+b; result=uint32_t(w); updateCarry=true; carry=(w>>32)!=0; arithmetic=true; signedWide=int64_t(int32_t(a))+int64_t(int32_t(b)); break; } // addc
    case 40: result=b-a; arithmetic=true; signedWide=int64_t(int32_t(b))-int64_t(int32_t(a)); break;
    case 104: result=0u-a; arithmetic=true; signedWide=-int64_t(int32_t(a)); break;
    case 136: { uint64_t w=uint64_t(uint32_t(~a))+b+uint64_t(oldCA); result=uint32_t(w); updateCarry=true; carry=(w>>32)!=0; arithmetic=true; signedWide=int64_t(int32_t(b))-int64_t(int32_t(a))-int64_t(!oldCA); break; } // subfe
    case 138: { uint64_t w=uint64_t(a)+b+uint64_t(oldCA); result=uint32_t(w); updateCarry=true; carry=(w>>32)!=0; arithmetic=true; signedWide=int64_t(int32_t(a))+int64_t(int32_t(b))+int64_t(oldCA); break; } // adde
    case 200: { uint64_t w=uint64_t(uint32_t(~a))+uint64_t(oldCA); result=uint32_t(w); updateCarry=true; carry=(w>>32)!=0; arithmetic=true; signedWide=-int64_t(int32_t(a))-int64_t(!oldCA); break; } // subfze
    case 202: { uint64_t w=uint64_t(a)+uint64_t(oldCA); result=uint32_t(w); updateCarry=true; carry=(w>>32)!=0; arithmetic=true; signedWide=int64_t(int32_t(a))+int64_t(oldCA); break; } // addze
    case 232: { uint64_t w=uint64_t(uint32_t(~a))+0xFFFFFFFFull+uint64_t(oldCA); result=uint32_t(w); updateCarry=true; carry=(w>>32)!=0; arithmetic=true; signedWide=-int64_t(int32_t(a))-1+int64_t(oldCA); break; } // subfme
    case 234: { uint64_t w=uint64_t(a)+0xFFFFFFFFull+uint64_t(oldCA); result=uint32_t(w); updateCarry=true; carry=(w>>32)!=0; arithmetic=true; signedWide=int64_t(int32_t(a))-1+int64_t(oldCA); break; } // addme
    case 24: result=((b&0x20u)!=0)?0u:(g[rt]<<(b&31u)); break; // slw rA,rS,rB
    case 26: result=std::countl_zero(g[rt]); break; // cntlzw rA,rS
    case 28: result=g[rt]&b; break;
    case 60: result=g[rt]&~b; break;
    case 124: result=~(g[rt]|b); break;
    case 412: result=g[rt]|~b; break;                    // orc
    case 476: result=~(g[rt]&b); break;                  // nand
    case 75: result=uint32_t((int64_t(int32_t(a))*int64_t(int32_t(b)))>>32); break; // mulhw
    case 11: result=uint32_t((uint64_t(a)*uint64_t(b))>>32); break; // mulhwu
    case 459: { // divwu
        if(b==0) { result=0; } else result=a/b;
        break;
    }
    case 491: { // divw; avoid host undefined behaviour for INT_MIN / -1
        const int32_t sa=int32_t(a), sb=int32_t(b);
        if(sb==0 || (sa==std::numeric_limits<int32_t>::min() && sb==-1)) result=0;
        else result=uint32_t(sa/sb);
        break;
    }
    case 235: result=uint32_t(int64_t(int32_t(a))*int64_t(int32_t(b))); break; // mullw
    case 266: result=a+b; arithmetic=true; signedWide=int64_t(int32_t(a))+int64_t(int32_t(b)); break;
    case 284: result=~(g[rt]^b); break;
    case 316: result=g[rt]^b; break;
    case 444: result=g[rt]|b; break;
    case 536: result=((b&0x20u)!=0)?0u:(g[rt]>>(b&31u)); break;
    case 792: { // sraw
        const unsigned sh=b&63u; const int32_t sa=int32_t(g[rt]);
        if(sh>=32) { result=sa<0?0xFFFFFFFFu:0u; carry=(sa<0 && g[rt]!=0); }
        else { result=uint32_t(sa>>sh); if(sh && sa<0) carry=(g[rt]&((1u<<sh)-1u))!=0; }
        updateCarry=true; break;
    }
    case 824: { // srawi
        const unsigned sh=(raw>>11)&31u; const int32_t sa=int32_t(g[rt]);
        result=uint32_t(sa>>sh); if(sh && sa<0) carry=(g[rt]&((1u<<sh)-1u))!=0;
        updateCarry=true; break;
    }
    case 922: result=uint32_t(int32_t(int16_t(g[rt]))); break;
    case 954: result=uint32_t(int32_t(int8_t(g[rt]))); break;
    case 339: { // mfspr
        const uint32_t spr=((raw>>16)&31u)|((raw>>6)&0x3E0u);
        if (spr >= 912 && spr <= 919) {
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged GQR read");
            result=registers.GQR[spr-912];
        } else if (spr >= 528 && spr <= 535) {
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged IBAT read");
            result=registers.IBAT[(spr-528)/2][(spr-528)%2];
        } else if (spr >= 536 && spr <= 543) {
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged DBAT read");
            result=registers.DBAT[(spr-536)/2][(spr-536)%2];
        } else switch(spr) {
        case 1: result=registers.XER; break;
        case 8: result=registers.LR; break;
        case 9: result=registers.CTR; break;
        case 26: if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged SRR0 read"); result=registers.SRR0; break;
        case 27: if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged SRR1 read"); result=registers.SRR1; break;
        case 18: if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged DSISR read"); result=registers.DSISR; break;
        case 19: if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged DAR read"); result=registers.DAR; break;
        case 22: if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged DEC read"); result=registers.DEC; break;
        case 25: if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged SDR1 read"); result=registers.SDR1; break;
        case 272: case 273: case 274: case 275:
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged SPRG read");
            result=registers.SPRG[spr-272]; break;
        case 920:
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged HID2 read");
            result=registers.HID2; break;
        case 1011:
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged HID4 read");
            result=registers.HID4; break;
        case 1008:
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged HID0 read");
            result=registers.HID0; break;
        case 1009:
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged HID1 read");
            result=registers.HID1; break;
        case 921:
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged WPAR read");
            result=registers.WPAR; break;
        case 952:
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged MMCR0 read");
            result=registers.MMCR0; break;
        case 956:
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged MMCR1 read");
            result=registers.MMCR1; break;
        case 953: case 954: case 957: case 958: {
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged performance-counter read");
            const unsigned index = spr == 953 ? 0u : spr == 954 ? 1u : spr == 957 ? 2u : 3u;
            result=registers.PMC[index]; break;
        }
        case 1017:
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged L2CR read");
            result=registers.L2CR; break;
        default:
            if (spr >= 560 && spr <= 575) {
                if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged SPR read");
                result=0; break;
            }
            throw CPUException(ExceptionKind::UnimplementedInstruction,lastInstructionPC,"Read from unsupported SPR");
        }
        break;
    }
    case 467: { // mtspr
        const uint32_t spr=((raw>>16)&31u)|((raw>>6)&0x3E0u);
        if (spr >= 912 && spr <= 919) {
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged GQR write");
            registers.GQR[spr-912]=g[rt];
        } else if (spr >= 528 && spr <= 535) {
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged IBAT write");
            registers.IBAT[(spr-528)/2][(spr-528)%2]=g[rt];
        } else if (spr >= 536 && spr <= 543) {
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged DBAT write");
            registers.DBAT[(spr-536)/2][(spr-536)%2]=g[rt];
        } else switch(spr) {
        case 1: registers.XER=g[rt]; break;
        case 8: registers.LR=g[rt]; break;
        case 9: registers.CTR=g[rt]; break;
        case 26: if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged SRR0 write"); registers.SRR0=g[rt]; break;
        case 27: if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged SRR1 write"); registers.SRR1=g[rt]; break;
        case 18: if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged DSISR write"); registers.DSISR=g[rt]; break;
        case 19: if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged DAR write"); registers.DAR=g[rt]; break;
        case 22: if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged DEC write"); registers.DEC=g[rt]; break;
        case 25: if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged SDR1 write"); registers.SDR1=g[rt]; break;
        case 284: if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged TBL write"); registers.TimeBase=(registers.TimeBase&0xFFFFFFFF00000000ull)|g[rt]; break;
        case 285: if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged TBU write"); registers.TimeBase=(uint64_t(g[rt])<<32)|(registers.TimeBase&0xFFFFFFFFull); break;
        case 272: case 273: case 274: case 275:
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged SPRG write");
            registers.SPRG[spr-272]=g[rt]; break;
        case 920:
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged HID2 write");
            registers.HID2=g[rt]; break;
        case 1011:
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged HID4 write");
            registers.HID4=g[rt]; break;
        case 1008:
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged HID0 write");
            registers.HID0=g[rt]; break;
        case 1009:
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged HID1 write");
            registers.HID1=g[rt]; break;
        case 921:
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged WPAR write");
            registers.WPAR=g[rt] & 0xFFFFFFE0u; break;
        case 952:
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged MMCR0 write");
            registers.MMCR0=g[rt]; break;
        case 956:
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged MMCR1 write");
            registers.MMCR1=g[rt]; break;
        case 953: case 954: case 957: case 958: {
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged performance-counter write");
            const unsigned index = spr == 953 ? 0u : spr == 954 ? 1u : spr == 957 ? 2u : 3u;
            registers.PMC[index]=g[rt]; break;
        }
        case 1017:
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged L2CR write");
            registers.L2CR=g[rt] & 0x7FFFFFFFu; break;
        case 512:
            if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged SPR 512 write");
            break;
        default:
            if (spr >= 560 && spr <= 575) {
                if (registers.MSR & CPURegisters::MSR_PR) throw CPUException(ExceptionKind::Program,lastInstructionPC,"Privileged SPR write");
                break;
            }
            throw CPUException(ExceptionKind::UnimplementedInstruction,lastInstructionPC,"Write to unsupported SPR");
        }
        return;
    }
    case 54: case 86: case 246: case 278: case 470: case 982:
        // Data/instruction cache state is not modeled. These operations have no
        // additional observable effect while all CPU accesses go to backing memory.
        return;
    case 1014: { // dcbz: clear the addressed 32-byte cache block in backing memory.
        const uint32_t address = ((ra ? g[ra] : 0u) + g[rb]) & ~uint32_t(31u);
        try {
            for (unsigned offset = 0; offset < 32; offset += 4)
                memory.Write32(address + offset, 0);
        } catch (const std::exception&) {
            throw CPUException(ExceptionKind::DataStorage,lastInstructionPC,
                               "dcbz cache block extends outside mapped memory");
        }
        return;
    }
    case 598: return; // sync: ordering is naturally sequential in this interpreter
    case 854: return; // eieio: no-op in the single-threaded memory model
    default: supported=false; break;
    }
    if(!supported)
        throw CPUException(ExceptionKind::UnimplementedInstruction,lastInstructionPC,"Unsupported integer extended opcode");

    if(updateCarry) registers.SetXERCarry(carry);
    if(oe && arithmetic) {
        const bool ov=signedWide>std::numeric_limits<int32_t>::max() ||
                      signedWide<std::numeric_limits<int32_t>::min();
        SetOverflow(ov);
    }
    const bool logicalToRA = xo==24 || xo==26 || xo==28 || xo==60 || xo==124 ||
        xo==284 || xo==316 || xo==412 || xo==444 || xo==476 || xo==536 ||
        xo==792 || xo==824 || xo==922 || xo==954;
    if (logicalToRA) g[ra]=result;
    else g[rt]=result;
    if(rc) UpdateCR0(result);
}
}
