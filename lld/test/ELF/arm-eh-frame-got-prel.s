@ REQUIRES: arm
@@ A CIE can refer to its personality routine's GOT entry with
@@ R_ARM_GOT_PREL (DW_EH_PE_indirect | DW_EH_PE_pcrel | DW_EH_PE_sdata4).
@ RUN: llvm-mc -filetype=obj -triple=armv7a-linux-gnueabi %s -o %t.o
@ RUN: ld.lld -shared %t.o -o %t.so
@ RUN: llvm-readelf -S -r %t.so > %t.txt
@ RUN: llvm-objdump --dwarf=frames %t.so >> %t.txt
@ RUN: FileCheck %s < %t.txt

@ CHECK:      .got PROGBITS [[#%x,GOT:]]
@ CHECK:      [[#%.8x,GOT]] {{.*}} R_ARM_GLOB_DAT {{.*}} pers
@ CHECK:      CIE
@ CHECK:      Augmentation: "zPR"
@ CHECK:      Personality Address: [[#%.16x,GOT]]

  .text
  .globl pers
pers:
  bx lr
f:
  bx lr

  .section .eh_frame,"a",%progbits
.Lcie:
  .long .Lcie_end - .Lcie_start
.Lcie_start:
  .long 0                    @ CIE id
  .byte 1                    @ version
  .asciz "zPR"
  .byte 1                    @ code alignment factor
  .byte 0x7c                 @ data alignment factor (-4)
  .byte 14                   @ return address register
  .byte 6                    @ augmentation data length
  .byte 0x9b                 @ personality encoding
  .long pers(GOT_PREL)
  .byte 0x1b                 @ FDE pointer encoding
  .byte 0x0c, 13, 0          @ DW_CFA_def_cfa sp, 0
  .p2align 2
.Lcie_end:
  .long .Lfde_end - .Lfde_start
.Lfde_start:
  .long .Lfde_start - .Lcie  @ CIE pointer
  .long f - .                @ initial location
  .long 4                    @ address range
  .byte 0                    @ augmentation data length
  .p2align 2
.Lfde_end:
