# REQUIRES: x86
# RUN: llvm-mc %s -o %t.o -filetype=obj -triple=x86_64-pc-linux

## A .gcc_except_table is live if a function whose LSDA it holds is, whether or
## not it is in a COMDAT group or has SHF_LINK_ORDER.

# RUN: ld.lld --gc-sections --print-gc-sections -u _Z3barv %t.o -o /dev/null | \
# RUN:   FileCheck %s --implicit-check-not=.gcc_except_table

# CHECK:      removing unused section {{.*}}.o:(.text._Z3foov)
# CHECK-NEXT: removing unused section {{.*}}.o:(.text._Z6comdatv)
# CHECK-NEXT: removing unused section {{.*}}.o:(.text._Z9linkorderv)
# CHECK-NEXT: removing unused section {{.*}}.o:(.text._Z3bazv)
# CHECK-NEXT: removing unused section {{.*}}.o:(.gcc_except_table._Z3foov)
# CHECK-NEXT: removing unused section {{.*}}.o:(.gcc_except_table._Z6comdatv)
# CHECK-NEXT: removing unused section {{.*}}.o:(.gcc_except_table._Z9linkorderv)

## .gcc_except_table, which holds the LSDAs of _Z3barv and _Z3bazv, is live
## because _Z3barv is.

# RUN: ld.lld --gc-sections --why-live=_Z3barv_lsda -u _Z3barv %t.o -o /dev/null | \
# RUN:   FileCheck %s --check-prefix=WHY
# WHY:      live symbol: {{.*}}.o:(_Z3barv_lsda)
# WHY-NEXT: >>> in live section: {{.*}}.o:(.gcc_except_table)
# WHY-NEXT: >>> LSDA of: {{.*}}.o:(.text._Z3barv)

## If the text sections are live, so are the .gcc_except_table sections.

# RUN: ld.lld --gc-sections --print-gc-sections -u _Z3foov -u _Z6comdatv \
# RUN:   -u _Z9linkorderv -u _Z3barv -u _Z3bazv %t.o -o /dev/null | \
# RUN:   FileCheck %s --check-prefix=KEEP

# KEEP-NOT: .gcc_except_table

.section .text._Z3foov,"ax",@progbits
.globl _Z3foov
_Z3foov:
  .cfi_startproc
  ret
  .cfi_lsda 0x1b,.Lexception0
  .cfi_endproc

.section .text._Z6comdatv,"axG",@progbits,_Z6comdatv,comdat
.globl _Z6comdatv
_Z6comdatv:
  .cfi_startproc
  ret
  .cfi_lsda 0x1b,.Lexception1
  .cfi_endproc

.section .text._Z9linkorderv,"ax",@progbits
.globl _Z9linkorderv
_Z9linkorderv:
  .cfi_startproc
  ret
  .cfi_lsda 0x1b,.Lexception2
  .cfi_endproc

.section .text._Z3barv,"ax",@progbits
.globl _Z3barv
_Z3barv:
  .cfi_startproc
  ret
  .cfi_lsda 0x1b,_Z3barv_lsda
  .cfi_endproc

.section .text._Z3bazv,"ax",@progbits
.globl _Z3bazv
_Z3bazv:
  .cfi_startproc
  ret
  .cfi_lsda 0x1b,.Lexception4
  .cfi_endproc

.section .gcc_except_table._Z3foov,"a",@progbits
.Lexception0:
  .byte 255

.section .gcc_except_table._Z6comdatv,"aG",@progbits,_Z6comdatv,comdat
.Lexception1:
  .byte 255

.section .gcc_except_table._Z9linkorderv,"ao",@progbits,_Z9linkorderv
.Lexception2:
  .byte 255

.section .gcc_except_table,"a",@progbits
_Z3barv_lsda:
  .byte 255
.Lexception4:
  .byte 255
