BROADWAY CPU — STAGE 3 UPGRADE
================================

This package extends the Stage 2 CPU core with scalar floating-point operations,
paired-single arithmetic, and floating-point memory instructions. It keeps the
existing Visual Studio C++20 project structure.

INSTALL
1. Back up your existing BroadwayCPU folder.
2. Extract this folder and copy its contents over the existing project files.
3. Open BroadwayCPU.slnx. Select x64 / Debug.
4. Build > Rebuild Solution, then run the tests.

NEW COVERAGE
- Scalar FP: fadd/fsub/fmul/fdiv, fused multiply-add/subtract variants, sqrt,
  select, sign operations, move, single-rounding, compares, conversions, FPSCR moves.
- FP memory: lfs/lfsu/lfd/lfdu/stfs/stfsu/stfd/stfdu.
- Paired-single arithmetic: add/sub/mul/div, fused operations, sums, scalar-multiply
  variants, sign operations, and merge operations.
- Existing integer, branch, load/store, register, memory and diagnostic systems retained.

IMPORTANT LIMITATIONS
This is an instruction-coverage expansion, not yet a fully validated PowerPC 750CL
implementation. Some FPSCR exception/rounding behaviour and paired-single details are
approximations. Paired-single quantized loads/stores (psq_l/psq_st), full MMU,
privileged exception vectors, cache operations, timing, and Wii hardware are not
implemented. Unsupported encodings raise an explicit CPUException rather than silently
pretending to execute. Do not use it to claim full Wii game compatibility yet.

The current FPR storage uses double values; paired-single values are packed as two
32-bit IEEE-754 lanes in the 64-bit representation. Scalar FP operations should be
used only with scalar values, paired-single operations only with packed lane values.


STAGE 4 ADDITIONS (this package)
-------------------------------
This package extends the Stage 3 interpreter with additional PowerPC integer
operations and indexed memory forms, including: lwzx/lwzux, lbzx/lbzux,
lhzx/lhzux, lhax/lhaux, stwx/stwux, stbx/stbux, sthx/sthux, lswx/lswi,
stswx/stswi, CR boolean operations (crand/cror/crxor/crnand/crnor/crandc/
creqv/crorc), mulhw/mulhwu, divw/divwu, orc and nand.

IMPORTANT ARCHITECTURAL LIMITATIONS
-----------------------------------
This is still an instruction-interpreter development project, not a validated
PowerPC 750CL/Broadway implementation. Some instruction families remain
unimplemented or simplified. In particular, this package does NOT claim full
coverage of privileged instructions, MMU/TLB/cache behaviour, exception
vectoring, reservation/coherency semantics, Gekko quantized paired-single
loads/stores (PSQ), exact FPSCR exception/rounding rules, or timing. Several
FPU and paired-single operations are approximations using host floating-point.
Do not use it as evidence that commercial Wii software will boot.

Build with Visual Studio 2026, x64, C++20. Run the built-in tests before
continuing. The original Stage 3 regression tests are retained.
