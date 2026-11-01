# OpenUEFI-Mod Specification & Architecture Blueprint

## 1. Overview & Community Objective
OpenUEFI-Mod is an open-source toolchain for automated UEFI BIOS analysis, microcode manipulation (including Xeon E5 v3 Turbo Unlock 06F0 injection and Coffee Lake microcode swapping), HII IFR de-suppression for hidden overclocking and memory tuning menus, and DXE driver injection (NVMe boot and Resizable BAR) with full volume CRC32 recalculation.

---

## 2. Firmware Interface Table (FIT) & Microcode Specification

### 2.1 FIT Entry Structure (16 Bytes)
The FIT pointer is located at physical memory address `0xFFFFFFC0` (ROM end - 0x40):
```
Offset  Size  Field Description
0x00    8     Address: 64-bit physical address of component (e.g., Microcode binary)
0x08    3     Size: 24-bit size in 16-byte chunks (Size * 16 = Byte length)
0x0B    1     Reserved
0x0C    2     Version: Component version (0x0100 for Microcode)
0x0D    1     Type: Component Type
              - 0x00: FIT Header ("_FIT_   ")
              - 0x01: Microcode Update
              - 0x02: ACM (Authenticated Code Module)
              - 0x07: BIOS Startup Module
              - 0x08: TPM Policy
0x0E    1     Checksum: If Type == 0x00, byte checksum making FIT sum % 256 == 0
```

### 2.2 Intel Microcode Header Structure
```
Offset  Size  Field Description
0x00    4     Header Version (0x00000001)
0x04    4     Update Revision
0x08    4     Date in BCD (YYYY-MM-DD)
0x0C    4     Processor Signature (CPUID: stepping, model, family)
0x10    4     Checksum (Sum of all 32-bit dwords == 0)
0x14    4     Loader Revision (0x00000001)
0x18    4     Processor Flags (Platform bitmask)
0x1C    4     Data Size (0 if uncompressed 2048 bytes, else byte count)
0x20    4     Total Size (0 if 2048 bytes, else total byte length)
0x24    12    Reserved
```

---

## 3. UEFI IFR (Internal Form Representation) De-Suppression

### 3.1 Opcode Encoding
UEFI HII Form packages in `Setup.pe32` encode UI visibility conditionals:
- `EFI_IFR_SUPPRESS_IF_OP` (`0x0A`): Suppresses the succeeding form or question if expression evaluates to TRUE.
- `EFI_IFR_GRAY_OUT_IF_OP` (`0x19`): Grays out the succeeding form or question if expression evaluates to TRUE.
- `EFI_IFR_END_OP` (`0x29`): Closes the suppression scope.
- `EFI_IFR_TRUE_OP` (`0x46`): Boolean TRUE literal.
- `EFI_IFR_FALSE_OP` (`0x47`): Boolean FALSE literal.

### 3.2 Neutralization Strategy
To unlock hidden BIOS menus (BCLK, vCore, memory timings, cTDP, fan curves):
1. Locate `EFI_IFR_SUPPRESS_IF` and `EFI_IFR_GRAY_OUT_IF` opcodes in the HII Form stream.
2. Replace conditional expression bytecode with `EFI_IFR_FALSE` (opcode `0x47`) followed by `EFI_IFR_END` or NOP padding.
3. Because the expression now unconditionally evaluates to FALSE, UEFI Setup displays all unlocked forms and questions.

---

## 4. DXE Driver Injection & Volume Checksum Recalculation

### 4.1 Firmware Volume (FV) and FFS Structure
- `EFI_FIRMWARE_VOLUME_HEADER`: Contains `_FVH` signature, HeaderLength, FileSystemGuid, Attributes, and Checksum.
- `EFI_FFS_FILE_HEADER`: Contains Name (GUID), IntegrityCheck (Header and File checksums), Type, Attributes, Size (3 bytes), State.

### 4.2 Injection Pipeline
1. Scan FV for `EFI_FV_FILETYPE_PAD` or free unallocated space.
2. Verify available space exceeds new driver FFS size (`NvmExpressDxe.ffs` or `ReBarUEFI.ffs`).
3. Insert FFS file aligned to 8-byte boundary.
4. Update/shrink trailing Pad file.
5. Recalculate FFS Header Checksum:
   ```c
   uint8_t sum = 0;
   for (size_t i = 0; i < sizeof(EFI_FFS_FILE_HEADER) - 1; i++) {
       sum += header_bytes[i];
   }
   header->IntegrityCheck.Checksum.Header = 0x100 - sum;
   ```
6. Recalculate Firmware Volume Header Checksum so 16-bit sum of header words == 0.
