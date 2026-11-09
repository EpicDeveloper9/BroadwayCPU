import struct
import sys


SOURCE_ADDRESS = 0x80007338
RESUME_ADDRESS = 0x80007500
EXPECTED_INSTRUCTIONS = (0x3C808002, 0x388410D4, 0x38A00080)
IPC_CALL_ADDRESS = 0x80007530
EXPECTED_IPC_CALL = 0x4800727D


def patch_dol(source_path, output_path):
    with open(source_path, "rb") as source:
        image = bytearray(source.read())

    def find_offset(address, byte_count):
        for section in range(7):
            file_offset = struct.unpack_from(">I", image, section * 4)[0]
            section_address = struct.unpack_from(">I", image, 0x48 + section * 4)[0]
            size = struct.unpack_from(">I", image, 0x90 + section * 4)[0]
            if section_address <= address and address + byte_count <= section_address + size:
                return file_offset + address - section_address
        raise ValueError(f"DOL has no text section containing 0x{address:08X}")

    patch_offset = find_offset(SOURCE_ADDRESS, 12)
    original = struct.unpack_from(">3I", image, patch_offset)
    if original != EXPECTED_INSTRUCTIONS:
        raise ValueError(f"Unexpected libogc code at 0x{SOURCE_ADDRESS:08X}: {original!r}")

    branch_displacement = RESUME_ADDRESS - SOURCE_ADDRESS
    branch_displacement -= 8
    branch = 0x48000000 | (branch_displacement & 0x03FFFFFC)
    struct.pack_into(">3I", image, patch_offset, 0x3FE0CC00, 0x63FF5000, branch)

    ipc_offset = find_offset(IPC_CALL_ADDRESS, 4)
    ipc_call = struct.unpack_from(">I", image, ipc_offset)[0]
    if ipc_call != EXPECTED_IPC_CALL:
        raise ValueError(
            f"Unexpected libogc IPC call at 0x{IPC_CALL_ADDRESS:08X}: 0x{ipc_call:08X}"
        )
    struct.pack_into(">I", image, ipc_offset, 0x60000000)
    with open(output_path, "wb") as output:
        output.write(image)

    print(
        f"Created GX-only test variant: skipped the libogc DSP bootstrap "
        f"and IPC client init; DSP/IPC hardware is not emulated by this patch."
    )


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: patch_dol.py input.dol output.dol")
    patch_dol(sys.argv[1], sys.argv[2])
