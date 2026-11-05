// REQUIRES: aarch64
/// Sync tables. Clang's sync-only CFI describes the frame in one row, which
/// then describes the whole function, including the call to the homogeneous
/// prologue helper before it. Other CFI is described at its call sites.
// RUN: llvm-mc -filetype=obj -triple=aarch64 %s -o %t.o
// RUN: ld.lld -z compact-unwind=sync %t.o -o %t -Ttext=0x210000
// RUN: llvm-readobj --unwind %t | FileCheck %s

// CHECK:      Start: 0x210000
// CHECK-NEXT: End: 0x21001C
// CHECK:      Mode: FP
// CHECK-NEXT: Convention: LLVM
// CHECK-NEXT: Registers: x19, x20, x29, x30
// CHECK-NEXT: FrameRecord: Yes
// CHECK-NEXT: O: 0
// CHECK-NEXT: N: 0
// CHECK-NEXT: RA: A
// CHECK:      0x210000: CFA=reg29+32: reg19=[CFA-8], reg20=[CFA-16], reg29=[CFA-32], reg30=[CFA-24], reg34=1

/// x19 is saved after the first call, so the two calls need different
/// descriptors.
// CHECK:      Start: 0x21001C
// CHECK:      0x21001c: CFA=reg31+32: reg29=[CFA-32], reg30=[CFA-24]
// CHECK:      Start: 0x21002C
// CHECK:      0x21002c: CFA=reg31+32: reg19=[CFA-16], reg29=[CFA-32], reg30=[CFA-24]

  .text
  .globl _start
_start:
hope:
  .cfi_startproc
  paciasp
  .cfi_negate_ra_state
  stp x29, x30, [sp, #-32]!
  bl prolog_helper
  .cfi_def_cfa w29, 32
  .cfi_offset w19, -8
  .cfi_offset w20, -16
  .cfi_offset w30, -24
  .cfi_offset w29, -32
  bl callee
  bl epilog_helper
  autiasp
  ret
  .cfi_endproc

late_save:
  .cfi_startproc
  stp x29, x30, [sp, #-32]!
  .cfi_def_cfa_offset 32
  .cfi_offset 29, -32
  .cfi_offset 30, -24
  mov x29, sp
  bl callee
  str x19, [sp, #16]
  .cfi_offset 19, -16
  bl callee
  brk #1
  .cfi_endproc

prolog_helper:
epilog_helper:
callee:
  ret
