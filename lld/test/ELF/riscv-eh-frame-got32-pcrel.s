# REQUIRES: riscv
## A CIE can refer to its personality routine's GOT entry with
## R_RISCV_GOT32_PCREL (DW_EH_PE_indirect | DW_EH_PE_pcrel | DW_EH_PE_sdata4).
# RUN: llvm-mc -filetype=obj -triple=riscv64 -mattr=-relax %s -o %t.o
# RUN: ld.lld -shared %t.o -o %t.so
# RUN: llvm-readelf -S -r %t.so > %t.txt
# RUN: llvm-objdump --dwarf=frames %t.so >> %t.txt
# RUN: FileCheck %s < %t.txt

# CHECK:      .got PROGBITS [[#%x,GOT:]]
# CHECK:      [[#%.16x,GOT+8]] {{.*}} R_RISCV_64 {{.*}} pers + 0
# CHECK:      CIE
# CHECK:      Augmentation: "zPR"
# CHECK:      Personality Address: [[#%.16x,GOT+8]]

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
  .word 0                    # CIE id
  .byte 1                    # version
  .asciz "zPR"
  .byte 1                    # code alignment factor
  .byte 0x78                 # data alignment factor (-8)
  .byte 1                    # return address register
  .byte 6                    # augmentation data length
  .byte 0x9b                 # personality encoding
  .word %gotpcrel(pers)
  .byte 0x1b                 # FDE pointer encoding
  .byte 0x0c, 2, 0           # DW_CFA_def_cfa sp, 0
  .p2align 2
.Lcie_end:
  .word .Lfde_end - .Lfde_start
.Lfde_start:
  .word .Lfde_start - .Lcie  # CIE pointer
  .word f - .                # initial location
  .word 4                    # address range
  .byte 0                    # augmentation data length
  .p2align 2
.Lfde_end:
