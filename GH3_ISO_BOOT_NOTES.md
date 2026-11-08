# RGHP52 ISO and boot investigation

The ISO reader added in `disc_image.cpp` operates on a raw, 1:1 Wii ISO without copying or modifying the source image. It validates the Wii magic, reads the disc ID/revision, parses all four partition-table groups with bounds checks, derives a title key from a supplied common key and ticket, and decrypts the Wii 0x8000-byte partition clusters into their 0x7C00-byte user-data streams. The common key is caller-provided; the application does not embed one.

Partition boot information includes the DOL and FST offsets/sizes and the apploader header. The FST parser resolves directory/file names and file offsets. A synthetic unencrypted image test exercises the table, boot block, apploader header, FST and bounds. A fixed AES-CBC vector verifies title-key derivation on Windows.

## RGHP52 observations

The supplied image is 4,699,979,776 bytes. Its header reports `RGHP52`, disc number 0, revision 1, and Wii magic `0x5D1C9EA3`. The partition table contains an update partition at `0x00050000` (type 1) and the game data partition at `0x0F800000` (type 0). The selected game partition reports:

- DOL logical data offset: `0x36B00`
- FST logical data offset: `0x45A200`, size/max size `0x78D4C` bytes (stored as `0x1E353` words)
- apploader logical data offset: `0x2440`
- apploader entry: `0x81200294`
- apploader code size: `0x1B1C`; trailer size: `0x32AA0`

These decrypted values are structurally consistent with the extracted game files. The reader currently does not verify the H0–H3 SHA-1 hierarchy or ticket/TMD signatures, and it does not support non-default common-key slots.

The actual partition FST parsed as 12,287 entries. It lists no files with `.rel`, `.dol`, `.elf`, or `.rso` extensions. The apploader code plus trailer is 214,460 bytes, matching the extracted `apploader.img` length after its 32-byte header. This only rules out modules explicitly named with those extensions in the FST; executable code can also be embedded in archives or generated/loaded by other mechanisms.

## Execution status

`gh3_iso_probe` can inspect the ISO and can perform a clearly labeled **direct-DOL diagnostic** by reading the DOL bytes through the partition reader. That diagnostic is not an ISO boot: it does not load/execute the apploader, construct the complete boot2/BI2 memory environment, provide DVD Interface callbacks, or initialize IOS/Starlet and Wii services.

The direct-DOL diagnostic ran from a clean CPU/GX reset. The word at `0x80252A38` (`0xFFA0004C`) is valid `mtfsb1 29`: operand 29 selects FPSCR bit 29 (NI), which maps to numeric mask `0x4`. It now sets that bit; the record form updates CR1. The instruction is recognized, dispatched, and covered by focused tests. The diagnostic executes it successfully and continues.

The latest clean direct-DOL probe stops after **712,044 successful instructions** at PC `0x8026BB80`, word `0x7C0018AC`, `dcbf r0,r3`. This cache-management instruction is decoded but not dispatched because BroadwayCPU has no data-cache model. No GX commands, EFB-to-XFB copies, or frames were submitted. No access to the implemented GX MMIO region was observed (0 reads/writes), and no DVD Interface exists to report as a supported or unsupported device transaction. Completed `mfspr`/`mtspr` accesses are counted by the probe; none had occurred before this stop. The last successful instruction is `mtspr ctr,r4` at `0x8026BB7C`.

Floating-point instructions now check `MSR[FP]`; when clear, they raise a distinct debugger-facing floating-point-unavailable fault and preserve the faulting PC and instruction state. The current CPU exception framework reports this as a host-side diagnostic exception; it does not yet enter the Broadway exception vector or update SRR/MSR as hardware would.

MMCR0 (952), MMCR1 (956), PMC1–4 (953, 954, 957, 958), and HID4 (1011) are now recognized as privileged SPR state, allowing the earlier startup probe to proceed. Performance counter event selection/increments and HID4 effects on implementation-specific hardware are not modeled; these register values are software-visible state only.

**No full ISO boot was achieved.** The apploader runner initializes the image-derived disc ID, Wii boot marker, MEM1 size, documented Wii board/clock values, and selected partition globals, then loads the apploader and invokes its entry with output pointers for init/main/close. It does not create OS-owned exception vectors, OSContext/thread structures, or an IOS environment. On RGHP52 it executes 360 instructions before the empty system-call vector faults. The CPU enters that vector architecturally, saving the following PC in SRR0, the old MSR in SRR1, applying exception MSR changes, and selecting low/high vector from MSR[IP]. At `0x00000C00` memory contains `0x00000000`; no handler has been installed.

The apploader disassembly shows `sc` after its data-cache block flush helper (`dcbst` loop). At the fault `r0=0x3c`, but the PowerPC `sc` encoding does not itself assign meaning to `r0`; it only raises the system-call exception. Available public Wii references establish that the retail launch occurs through System Menu/BS2 and that apploader callbacks and DVD reads are driven by that running environment. They do not establish a Wii service mapping for number `0x3c` or provide the required handler code. Consequently, there is not enough evidence to implement that handler or claim an OS service response. Until the actual Revolution OS handler is supplied or independently specified, init/main/close are not called, the DI read callbacks are not exercised, the game DOL is not loaded, and the game entry is not reached. Latest measurements: 360 instructions counted including `sc`; GX commands 0; EFB-to-XFB copies 0; frames submitted 0. No genuine image is produced.

The syscall exception path has focused tests for decode/dispatch, saved PC/MSR, vector prefix, masked MSR bits, register preservation, and instruction count. Added low-memory initialization tests cover the documented boot marker, disc-header bytes, MEM1 and clock fields, partition selection, and app type. The current CPU's other host-reported faults are still not a complete architectural exception subsystem. The apploader path still lacks the actual System Menu/BS2 runtime, its Revolution OS system-call handler and OS structures, BI2/FST apploader callback sequence, and IOS/Starlet services. WiiBrew documents the BS2 calls and partition/apploader flow ([System Menu/BS2](https://wiibrew.org/wiki/System_Menu/BS2)), exception vectors and OS initialization ([Revolution OS](https://wiibrew.org/wiki/Revolution_OS)), and boot-owned low-memory fields ([memory map](https://wiibrew.org/wiki/Memory_map)); YAGCD documents the apploader ABI ([chapter 18](https://hitmen.c02.at/files/yagcd/yagcd/chap18.html)).
