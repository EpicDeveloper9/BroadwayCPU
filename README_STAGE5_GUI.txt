BROADWAYCPU — STAGE 5: WINDOWS GRAPHICAL WORKBENCH
==================================================

This update replaces the old console test runner with a native Windows desktop
application. It keeps the existing C++20 CPU implementation and adds a Win32
GUI using standard Windows controls and GDI drawing. No third-party UI library
is required.

WHAT'S NEW
----------
- Native Windows application; no console window is created.
- Dark CPU workbench layout.
- Load executable dialog for DOL, 32-bit big-endian ELF, and raw binary files.
- DOL text/data section loader, BSS zero-fill, and entry-point setup.
- ELF PT_LOAD segment loader and entry-point setup.
- Raw binaries are loaded at 0x80004000.
- Wii MEM1 cached (0x80000000) and uncached (0xC0000000) aliases map to the
  same physical memory region in the flat memory implementation.
- Run, Pause, Single Step, and Reset controls.
- GPR and FPR register views, plus PC/LR/CTR/XER/CR/MSR/FPSCR.
- Instruction disassembly listing using the current decoder's mnemonic names.
- Big-endian memory hex view.
- CPU exceptions are caught and shown in the application status line.
- Keyboard shortcuts: Ctrl+O, F5, F10, Ctrl+R.

BUILD / INSTALL
---------------
1. Back up your current BroadwayCPU project folder.
2. Extract this ZIP.
3. Copy the contents of BroadwayCPU_Stage5_GUI over your existing project,
   replacing matching source and project files.
4. Open BroadwayCPU.slnx in Visual Studio 2026.
5. Select x64 / Debug.
6. Build > Rebuild Solution.
7. Run BroadwayCPU.exe. It opens as a normal Windows application.

The project now uses the Windows subsystem and links Comdlg32 and Comctl32.
The GUI source is main.cpp; the CPU implementation remains in the separate
instruction-unit source files.

IMPORTANT LIMITATIONS
---------------------
This is a graphical debugger/workbench around the current developing CPU core,
not a complete Wii emulator. Loading a DOL or ELF only loads its segments and
sets the CPU entry point. It does not provide Hollywood, Starlet/IOS, GPU/GX,
DVD, audio, controller, IPC, full MMU/TLB, cache-coherency, exception-vector,
or timing emulation. The current instruction implementation still has gaps and
some floating-point / paired-single behaviour is approximate. Commercial Wii
games are not expected to boot reliably at this stage.

The disassembly pane currently displays decoded instruction mnemonics, not a
full symbolic disassembler with operand formatting or symbols. There are no
breakpoints yet. Run executes in bounded batches on the UI thread so the window
can refresh between batches; this is intended for early debugging, not cycle-
accurate performance measurement.

The Win32 GUI could not be compiled in this Linux build environment. The
portable CPU core was compiled with a C++20 compiler, and a focused regression
test confirmed that a high MEM1-alias address can be fetched and executed.
Please rebuild with Visual Studio on Windows and share any compiler output if
anything needs adjusting.
