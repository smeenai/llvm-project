// REQUIRES: aarch64
/// A CIE can refer to its personality routine's GOT entry with
/// R_AARCH64_GOTPCREL32 (DW_EH_PE_indirect | DW_EH_PE_pcrel | DW_EH_PE_sdata4).
// RUN: llvm-mc -filetype=obj -triple=aarch64 %s -o %t.o
// RUN: ld.lld -shared %t.o -o %t.so
// RUN: llvm-readelf -S -r %t.so > %t.txt
// RUN: llvm-objdump --dwarf=frames %t.so >> %t.txt
// RUN: FileCheck %s < %t.txt

// CHECK:      .got PROGBITS [[#%x,GOT:]]
// CHECK:      [[#%.16x,GOT]] {{.*}} R_AARCH64_GLOB_DAT {{.*}} pers + 0
// CHECK:      CIE
// CHECK:      Augmentation: "zPR"
// CHECK:      Personality Address: [[#%.16x,GOT]]

/// -z compact-unwind's personality table refers to the same GOT entry.
// RUN: ld.lld -shared -z compact-unwind %t.o -o %t.cu.so
// RUN: llvm-readelf -S %t.cu.so > %t.cu.txt
// RUN: llvm-readobj --unwind %t.cu.so >> %t.cu.txt
// RUN: FileCheck %s --check-prefix=CU < %t.cu.txt

// CU:      .got PROGBITS [[#%x,GOT:]]
// CU:      Personalities [
// CU-NEXT:   0: 0x[[#%x,GOT]]
// CU-NEXT: ]

  .text
  .globl pers
pers:
  ret
f:
  ret

  .section .eh_frame,"a",@progbits
.Lcie:
  .word .Lcie_end - .Lcie_start
.Lcie_start:
  .word 0                    // CIE id
  .byte 1                    // version
  .asciz "zPR"
  .byte 1                    // code alignment factor
  .byte 0x78                 // data alignment factor (-8)
  .byte 30                   // return address register
  .byte 6                    // augmentation data length
  .byte 0x9b                 // personality encoding
  .word %gotpcrel(pers)
  .byte 0x1b                 // FDE pointer encoding
  .byte 0x0c, 31, 0          // DW_CFA_def_cfa sp, 0
  .p2align 2
.Lcie_end:
  .word .Lfde_end - .Lfde_start
.Lfde_start:
  .word .Lfde_start - .Lcie  // CIE pointer
  .word f - .                // initial location
  .word 4                    // address range
  .byte 0                    // augmentation data length
  .p2align 2
.Lfde_end:
