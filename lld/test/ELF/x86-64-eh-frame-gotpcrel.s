# REQUIRES: x86
## A CIE can refer to its personality routine's GOT entry with
## R_X86_64_GOTPCREL (DW_EH_PE_indirect | DW_EH_PE_pcrel | DW_EH_PE_sdata4).
# RUN: llvm-mc -filetype=obj -triple=x86_64 %s -o %t.o
# RUN: ld.lld -shared %t.o -o %t.so
# RUN: llvm-readelf -S -r %t.so > %t.txt
# RUN: llvm-objdump --dwarf=frames %t.so >> %t.txt
# RUN: FileCheck %s < %t.txt

# CHECK:      .got PROGBITS [[#%x,GOT:]]
# CHECK:      [[#%.16x,GOT]] {{.*}} R_X86_64_GLOB_DAT {{.*}} pers + 0
# CHECK:      CIE
# CHECK:      Augmentation: "zPR"
# CHECK:      Personality Address: [[#%.16x,GOT]]

  .text
  .globl pers
pers:
  ret
f:
  ret

  .section .eh_frame,"a",@unwind
.Lcie:
  .long .Lcie_end - .Lcie_start
.Lcie_start:
  .long 0                    # CIE id
  .byte 1                    # version
  .asciz "zPR"
  .byte 1                    # code alignment factor
  .byte 0x78                 # data alignment factor (-8)
  .byte 16                   # return address register
  .byte 6                    # augmentation data length
  .byte 0x9b                 # personality encoding
  .long pers@GOTPCREL
  .byte 0x1b                 # FDE pointer encoding
  .byte 0x0c, 7, 8           # DW_CFA_def_cfa rsp, 8
  .p2align 2
.Lcie_end:
  .long .Lfde_end - .Lfde_start
.Lfde_start:
  .long .Lfde_start - .Lcie  # CIE pointer
  .long f - .                # initial location
  .long 1                    # address range
  .byte 0                    # augmentation data length
  .p2align 2
.Lfde_end:
