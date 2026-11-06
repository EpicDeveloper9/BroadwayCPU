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

    const uint32_t xo=(raw>>1)&0x1FFu;
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

    const bool oe=(raw&0x400u)!=0;
    const bool rc=(raw&1u)!=0;
    const bool oldCA=registers.GetXERCarry();
    uint32_t result=0;
    bool supported=true, updateCarry=false, carry=false;
    bool arithmetic=false;
    int64_t signedWide=0;

    switch(xo) {
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
    case 24: result=((b&0x20u)!=0)?0u:(a<<(b&31u)); break; // slw
    case 26: result=std::countl_zero(a); break; // cntlzw
    case 28: result=a&b; break;
    case 60: result=a&~b; break;
    case 124: result=~(a|b); break;
    case 412: result=a|~b; break;                    // orc
    case 476: result=~(a&b); break;                  // nand
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
    case 284: result=~(a^b); break;
    case 316: result=a^b; break;
    case 444: result=a|b; break;
    case 536: result=((b&0x20u)!=0)?0u:(a>>(b&31u)); break;
    case 792: { // sraw
        const unsigned sh=b&63u; const int32_t sa=int32_t(a);
        if(sh>=32) { result=sa<0?0xFFFFFFFFu:0u; carry=(sa<0 && a!=0); }
        else { result=uint32_t(sa>>sh); if(sh && sa<0) carry=(a&((1u<<sh)-1u))!=0; }
        updateCarry=true; break;
    }
    case 824: { // srawi
        const unsigned sh=(raw>>11)&31u; const int32_t sa=int32_t(a);
        result=uint32_t(sa>>sh); if(sh && sa<0) carry=(a&((1u<<sh)-1u))!=0;
        updateCarry=true; break;
    }
    case 922: result=uint32_t(int32_t(int16_t(a))); break;
    case 954: result=uint32_t(int32_t(int8_t(a))); break;
    case 339: { // mfspr
        const uint32_t spr=((raw>>16)&31u)|((raw>>6)&0x3E0u);
        switch(spr) {
        case 1: result=registers.XER; break;
        case 8: result=registers.LR; break;
        case 9: result=registers.CTR; break;
        default: throw CPUException(ExceptionKind::UnimplementedInstruction,lastInstructionPC,"Read from unsupported SPR");
        }
        break;
    }
    case 467: { // mtspr
        const uint32_t spr=((raw>>16)&31u)|((raw>>6)&0x3E0u);
        switch(spr) {
        case 1: registers.XER=g[rt]; break;
        case 8: registers.LR=g[rt]; break;
        case 9: registers.CTR=g[rt]; break;
        default: throw CPUException(ExceptionKind::UnimplementedInstruction,lastInstructionPC,"Write to unsupported SPR");
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
    g[rt]=result;
    if(rc) UpdateCR0(result);
}
}
