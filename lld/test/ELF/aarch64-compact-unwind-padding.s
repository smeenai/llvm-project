// REQUIRES: aarch64
/// Alignment padding between functions belongs to the region before it, which
/// for a region with an epilogue means growing its tail. Longer padding
/// gets an entry without unwind information.
// RUN: llvm-mc -filetype=obj -triple=aarch64 %s -o %t.o
// RUN: ld.lld -z compact-unwind %t.o -o %t -Ttext=0x210000
// RUN: llvm-readobj --unwind %t | FileCheck %s

// CHECK:      Start: 0x210000
// CHECK-NEXT: End: 0x210004
// CHECK:      Mode: NULL
// CHECK:      Start: 0x210004
// CHECK-NEXT: End: 0x21000C
// CHECK:      Mode: SP
// CHECK:      Start: 0x21000C
// CHECK-NEXT: End: 0x210024
// CHECK:      Mode: NULL
// CHECK:      Start: 0x210024
// CHECK-NEXT: End: 0x21002C
// CHECK:      Mode: SP
// CHECK:      Start: 0x21002C
// CHECK-NEXT: End: 0x210084
// CHECK:      Mode: NULL

  .text
  .globl _start
  .p2align 4
/// 4 instructions, then 4 NOPs before short: tail length = 1 + 4.
_start:
  .cfi_startproc
  stp x29, x30, [sp, #-16]!
  .cfi_def_cfa_offset 16
  .cfi_offset w30, -8
  .cfi_offset w29, -16
  bl short
  ldp x29, x30, [sp], #16
  .cfi_def_cfa_offset 0
  .cfi_restore w30
  .cfi_restore w29
  ret
  .cfi_endproc

  .p2align 5
/// 4 instructions, then 20 NOPs before far, more than the tail length can cover.
short:
  .cfi_startproc
  stp x29, x30, [sp, #-16]!
  .cfi_def_cfa_offset 16
  .cfi_offset w30, -8
  .cfi_offset w29, -16
  bl far
  ldp x29, x30, [sp], #16
  .cfi_def_cfa_offset 0
  .cfi_restore w30
  .cfi_restore w29
  ret
  .cfi_endproc

  .p2align 7
far:
  .cfi_startproc
  ret
  .cfi_endproc
