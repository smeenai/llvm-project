# REQUIRES: x86
## Test that functions with LSDAs fold only if their LSDAs are equivalent and
## are interpreted by the same personality routine.

## Test REL.
# RUN: llvm-mc -filetype=obj -triple=i386 %s -o %t1.o
# RUN: ld.lld --icf=all %t1.o -o /dev/null --print-icf-sections | FileCheck %s --implicit-check-not=removing
## Test RELA.
# RUN: llvm-mc -filetype=obj -triple=x86_64 %s -o %t2.o
# RUN: ld.lld --icf=all %t2.o -o /dev/null --print-icf-sections | FileCheck %s --implicit-check-not=removing

## The LSDAs of a and b fold, so a and b fold too. c's LSDA differs, and d's
## and e's are different LSDAs in one section. f's LSDA folds with a's, but f's
## personality routine differs.
# CHECK:      selected section {{.*}}.o:(.text.a)
# CHECK-NEXT:   removing identical section {{.*}}.o:(.text.b)
# CHECK:      selected section {{.*}}.o:(.gcc_except_table.a)
# CHECK-NEXT:   removing identical section {{.*}}.o:(.gcc_except_table.b)
# CHECK-NEXT:   removing identical section {{.*}}.o:(.gcc_except_table.f)

## An empty address-significance table, so that the LSDAs can fold.
.addrsig

.globl a, b, c, d, e, f
.section .text.a,"ax",@progbits
a:
  .cfi_startproc
  .cfi_personality 0x1b, pers1
  .cfi_lsda 0x1b, .La
  ret
  .cfi_endproc

.section .text.b,"ax",@progbits
b:
  .cfi_startproc
  .cfi_personality 0x1b, pers1
  .cfi_lsda 0x1b, .Lb
  ret
  .cfi_endproc

.section .text.c,"ax",@progbits
c:
  .cfi_startproc
  .cfi_personality 0x1b, pers1
  .cfi_lsda 0x1b, .Lc
  ret
  .cfi_endproc

.section .text.d,"ax",@progbits
d:
  .cfi_startproc
  .cfi_personality 0x1b, pers1
  .cfi_lsda 0x1b, .Ld
  ret
  .cfi_endproc

.section .text.e,"ax",@progbits
e:
  .cfi_startproc
  .cfi_personality 0x1b, pers1
  .cfi_lsda 0x1b, .Le
  ret
  .cfi_endproc

.section .text.f,"ax",@progbits
f:
  .cfi_startproc
  .cfi_personality 0x1b, pers2
  .cfi_lsda 0x1b, .Lf
  ret
  .cfi_endproc

.text
pers1:
  nop
  ret
pers2:
  nop
  nop
  ret

.section .gcc_except_table.a,"a",@progbits
.La:
  .byte 0xff, 1

.section .gcc_except_table.b,"a",@progbits
.Lb:
  .byte 0xff, 1

.section .gcc_except_table.c,"a",@progbits
.Lc:
  .byte 0xff, 2

.section .gcc_except_table.de,"a",@progbits
.Ld:
  .byte 0xff, 3
.Le:
  .byte 0xff, 3

.section .gcc_except_table.f,"a",@progbits
.Lf:
  .byte 0xff, 1
