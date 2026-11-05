// REQUIRES: aarch64
/// CIEs with the 'N' augmentation (non-hint PAuth_LR instructions) are parsed
/// like any other, e.g. when --icf looks for LSDAs.
// RUN: llvm-mc -filetype=obj -triple=aarch64 %s -o %t.o
// RUN: ld.lld --icf=all %t.o -o %t
// RUN: llvm-readelf -S %t | FileCheck %s

// CHECK: .eh_frame

.globl _start
.section .text.a,"ax"
_start:
.cfi_startproc
.cfi_non_hint_pauth_frame
ret
.cfi_endproc

.section .text.b,"ax"
f:
.cfi_startproc
.cfi_non_hint_pauth_frame
ret
.cfi_endproc
