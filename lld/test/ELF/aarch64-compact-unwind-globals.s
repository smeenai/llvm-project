// REQUIRES: aarch64
/// More distinct descriptors than the global table holds (3755): the rest are
/// local to their pages, and a PARTIAL descriptor that isn't global can name a
/// layout descriptor local to its page.
// RUN: llvm-mc -filetype=obj -triple=aarch64 %s -o %t.o
// RUN: ld.lld -z compact-unwind %t.o -o %t -Ttext=0x210000
// RUN: llvm-readobj --unwind %t | FileCheck %s

// CHECK:      GlobalDescriptorCount: 3755
// CHECK:      Start: 0x210010
// CHECK-NEXT: End: 0x210018
// CHECK-NEXT: Descriptor: 0x{{[0-9a-f]+}} (local {{[0-9]+}})
// CHECK-NEXT: Mode: PARTIAL SP
// CHECK-NEXT: Layout: local {{[0-9]+}}
// CHECK-NEXT: Convention: GCC
// CHECK-NEXT: Registers: x19, x20, x29, x30, d15
// CHECK-NEXT: Saved: x29, x30, d15
// CHECK:      0x210010: CFA=reg31+48: reg29=[CFA-48], reg30=[CFA-40], reg79=[CFA-16]

  .text
  .globl _start
  .p2align 2
_start:
  ret

/// GCC records each save, so the state after d15 is saved is PARTIAL.
gcc_partial:
  .cfi_startproc
  stp x29, x30, [sp, #-48]!
  .cfi_def_cfa_offset 48
  .cfi_offset 29, -48
  .cfi_offset 30, -40
  mov x29, sp
  str d15, [sp, #32]
  .cfi_offset 79, -16
  fmov s15, s0
  stp x19, x20, [sp, #16]
  .cfi_offset 19, -32
  .cfi_offset 20, -24
  mov w20, w0
  bl _start
  ldp x19, x20, [sp, #16]
  ldr d15, [sp, #32]
  ldp x29, x30, [sp], #48
  .cfi_restore 30
  .cfi_restore 29
  .cfi_restore 19
  .cfi_restore 20
  .cfi_restore 79
  .cfi_def_cfa_offset 0
  ret
  .cfi_endproc

/// 3755 distinct frame sizes, three functions each, which outrank every other
/// descriptor.
.rept 3
.set n, 1
.rept 3755
  .cfi_startproc
  .cfi_def_cfa_offset 16 * n
  ret
  .cfi_endproc
.set n, n + 1
.endr
.endr
