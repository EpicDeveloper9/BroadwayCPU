# Guitar Hero III (RGHP52) static instruction scan

## Input and scope

The scan used the Europe Revision 1 executable at `DATA/sys/main.dol`, with the disc ID `RGHP52`. The input file is 4,339,360 bytes. It scanned only the seven DOL text-section entries from the header; empty entries were skipped. Data sections, BSS, the UPDATE partition executable, RELs and other files are not part of these instruction totals.

The scanner reads words in big-endian order, validates each non-empty text section's file offset, file bounds, word alignment, address alignment and file-range overlap, and rejects non-word-sized sections. The present image has 3,615,328 text bytes, or 903,832 words. Header inspection shows text section 0 at file offset `0x100`, address `0x80004000`, size `9,440`; text section 1 at `0x25E0`, address `0x80043720`, size `3,605,888`; the remaining five text entries are empty.

## Results

| Measure | Result |
|---|---:|
| DOL bytes | 4,339,360 |
| Text bytes scanned | 3,615,328 |
| Instruction words scanned | 903,832 |
| Distinct raw encodings | 183,933 |
| Distinct decoder labels | 171 |
| Occurrences decoded/recognized | 902,025 |
| Valid encoding occurrences | 902,025 |
| Known invalid encoding occurrences | 1,656 |
| Validity unclassified occurrences | 151 |
| Occurrences with an execution dispatch path | 902,011 |
| Occurrences accepted by the decoder's implemented flag | 902,011 |
| Occurrences with audited implemented semantics | 901,820 |
| Occurrences without audited implemented semantics | 2,012 |
| Occurrences in mnemonic families represented in tests | 490,991 |

The main CSV has one row per raw encoding and frequency. Its columns separately report decoder recognition, validity (`yes`, `no`, or `unclassified`), execution dispatch path, decoder `implemented` flag, an audited semantics result, and a family-level test indicator. The companion `.families.csv` groups those classifications by primary opcode, extended/operand field, and mnemonic. `family-test` means the mnemonic family is represented in automated semantic tests; it does not assert every encoding variant or operand combination was tested. `implemented_semantics` excludes `sc` because it still lacks architectural exception entry, and excludes unsupported SPR/TBR selectors even though the generic SPR opcode itself decodes and dispatches. The scanner's semantics audit is intentionally conservative and does not prove edge-case completeness.

## Largest unimplemented or undecoded families

| Opcode / XO | Decoder label | Count | Example encoding | Decoded | Notes |
|---|---|---:|---|---|---|
| 0 / 0 | illegal | 1,654 | `0x00000000` | no | Known reserved primary opcode; text data may contribute |
| 31 / 467 | mtspr | 148 | `0x7C12FBA6` | yes | Recognized opcode, but selected SPR is unsupported |
| 4 / 6 | unsupported | 65 | `0x11C0380C` | unclassified | Unmapped paired-single encoding |
| 4 / 7 | unsupported | 65 | `0x11C0180E` | unclassified | Unmapped paired-single encoding |
| 31 / 339 | mfspr | 40 | `0x7DFF42A6` | yes | Recognized opcode, but selected SPR is unsupported |
| 31 / 54 | dcbst | 6 | `0x7C00186C` | yes | Cache operation is not modeled |
| 31 / 982 | unsupported | 4 | `0x7C0017AC` | unclassified | Unknown X-form family |
| 31 / 278 | dcbt | 4 | `0x7C0B322C` | yes | Cache operation is not modeled |
| 17 / 0 | sc | 3 | `0x44000002` | yes | Decoded, but exception entry is not implemented |
| 31 / 86 | dcbf | 3 | `0x7C0018AC` | yes | Cache operation is not modeled |
| 31 / 1014 | dcbz | 1 | `0x7C001FEC` | yes | Cache operation is not modeled |

The current scan contains one `mtfsb1` occurrence (`0xFFA0004C`), now classified as decoded, valid, dispatchable, implemented, and represented by focused tests. The totals reconcile as 902,025 recognized/valid + 1,656 known invalid + 151 unclassified = 903,832 scanned words. Of the 2,012 occurrences without audited semantics, 1,656 are known invalid, 151 have unknown validity, and 205 are recognized but not implemented semantically. The scan includes three `dcbf` occurrences; these remain unsupported pending a data-cache model. CSVs preserve per-encoding and family-level classifications.

## Limits

This is a static scan, not a disassembly proof or runtime trace. Bytes embedded in DOL text sections (jump tables, constants, padding or other data) can resemble valid instructions and are counted as words. It cannot establish whether any instruction executes. Segment, BAT, GQR, SPRG, and time-base register storage/access now exists, but BAT/MMU translation and a hardware-driven time-base clock are not yet modeled. The extracted `DATA` tree has not yet had a complete dynamic-module/signature and loader audit, and this scan does not include REL contents. No game runtime attempt is represented by these counts.
