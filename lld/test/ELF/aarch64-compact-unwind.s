// REQUIRES: aarch64
/// -z compact-unwind replaces .eh_frame and the .eh_frame_hdr search table
/// with a compact unwind table in .eh_frame_hdr. This checks the table's
/// regions, escapes, code without unwind information and LSDAs; the other
/// aarch64-compact-unwind-*.s tests check frame shapes, personality routines,
/// PAuth_LR and padding.

// RUN: llvm-mc -filetype=obj -triple=aarch64 %s -o %t.o
// RUN: ld.lld -z compact-unwind %t.o -o %t -Ttext=0x210000 -Tdata=0x230000
// RUN: llvm-readelf -S -l -s %t 2>&1 | \
// RUN:   FileCheck %s --check-prefix=SEC --implicit-check-not=warning:
// RUN: llvm-readobj --unwind %t | FileCheck %s --check-prefix=ASYNC
/// Sync-only tables say so in their header. aarch64-compact-unwind-frames.s
/// checks how their descriptors differ.
// RUN: ld.lld -z compact-unwind=sync %t.o -o %t.sync -Ttext=0x210000 -Tdata=0x230000
// RUN: llvm-readobj --unwind %t.sync | FileCheck %s --check-prefix=SYNC

/// Big-endian tables say the same.
// RUN: llvm-mc -filetype=obj -triple=aarch64_be %s -o %t.be.o
// RUN: ld.lld -z compact-unwind %t.be.o -o %t.be -Ttext=0x210000 -Tdata=0x230000
// RUN: llvm-readobj --unwind %t.be | FileCheck %s --check-prefix=ASYNC

// RUN: not ld.lld -z compact-unwind=bogus %t.o -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=ERR
// RUN: not ld.lld -r -z compact-unwind %t.o -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=ERR-R
// ERR: error: unknown -z compact-unwind= value 'bogus'
// ERR-R: error: -r and -z compact-unwind may not be used together

/// .eh_frame holds only its terminator, and PT_GNU_EH_FRAME covers the table.
// SEC:     .eh_frame_hdr PROGBITS [[HDR:[0-9a-f]+]]
// SEC-NEXT: .eh_frame PROGBITS {{[0-9a-f]+}} {{[0-9a-f]+}} 000004
// SEC:     GNU_EH_FRAME {{.*}} 0x{{0*}}[[HDR]]

/// References into .eh_frame, like crtbegin.o's __EH_FRAME_LIST__, resolve to
/// the terminator even when the referring object's records would come later.
// RUN: echo '.section .eh_frame,"a",@progbits; list: .text; f: .cfi_startproc; \
// RUN:   ret; .cfi_endproc; .data; .xword list' | \
// RUN:   llvm-mc -filetype=obj -triple=aarch64 - -o %t.ref.o
// RUN: ld.lld -z compact-unwind %t.o %t.ref.o -o %t.ref
// RUN: llvm-readelf -S -s %t.ref | FileCheck %s --check-prefix=REF
// REF: .eh_frame PROGBITS [[#%.16x,EH:]]
// REF: [[#%.16x,EH]] 0 NOTYPE LOCAL DEFAULT [[#]] list

// ASYNC:      Flags [ (0x0)
// ASYNC:      0: 0x230000
// ASYNC:      Start: 0x210000
// ASYNC-NEXT: End: 0x21000C
// ASYNC:      Mode: NULL
// ASYNC:      Start: 0x21000C
// ASYNC-NEXT: End: 0x210018
// ASYNC:      Mode: NULL
// ASYNC:      Personality: 1
// ASYNC:      Start: 0x210018
// ASYNC-NEXT: End: 0x21001C
// ASYNC:      Mode: FP
// ASYNC:      Personality: 1
// ASYNC:      Start: 0x21001C
// ASYNC-NEXT: End: 0x210024
// ASYNC:      Mode: SP
// ASYNC:      Personality: 1
// ASYNC:      Start: 0x210024
// ASYNC-NEXT: End: 0x210028
// ASYNC:      Mode: NULL
// ASYNC:      Personality: 1
// ASYNC:      Start: 0x210028
// ASYNC-NEXT: End: 0x210034
// ASYNC:      Mode: Escape
// ASYNC:      Start: 0x210034
// ASYNC-NEXT: End: 0x21003C
// ASYNC:      Mode: NoInfo
// ASYNC:      Start: 0x21003C
// ASYNC-NEXT: End: 0x210044
// ASYNC:      Mode: Escape
// ASYNC-NEXT: Augmentation: S
// ASYNC:      Kind: LSDA (0x1)
// ASYNC:      0x21000c: 0x240008

// SYNC:      Flags [ (0x1)
// SYNC-NEXT: SyncOnly (0x1)

  .text
  .globl _start
  .p2align 2
/// No CFI rows.
_start:
  .cfi_startproc
  bl frame
  bl nocfi
  ret
  .cfi_endproc

/// An LLVM frame record with x19, with a personality routine and an LSDA.
  .p2align 2
frame:
  .cfi_startproc
  .cfi_personality 0x9b, DW.ref.pers
  .cfi_lsda 0x1b, .Lexcept
  stp x29, x30, [sp, #-32]!
  str x19, [sp, #16]
  mov x29, sp
  .cfi_def_cfa w29, 32
  .cfi_offset w19, -16
  .cfi_offset w30, -24
  .cfi_offset w29, -32
  bl expr
  .cfi_def_cfa wsp, 32
  ldr x19, [sp, #16]
  ldp x29, x30, [sp], #32
  .cfi_def_cfa_offset 0
  .cfi_restore w19
  .cfi_restore w30
  .cfi_restore w29
  ret
  .cfi_endproc

/// A CFA expression needs an escape program.
  .p2align 2
expr:
  .cfi_startproc
  sub sp, sp, #16
  .cfi_escape 0x0f, 0x03, 0x8f, 0x10, 0x06 // DW_CFA_def_cfa_expression: [sp + 16] deref
  add sp, sp, #16
  .cfi_def_cfa wsp, 0
  ret
  .cfi_endproc

/// Code without an FDE has no unwind information.
nocfi:
  ret
  ret

/// So do signal frames, as escapes, and FDEs without code.
  .p2align 2
signal:
  .cfi_startproc
  .cfi_signal_frame
  nop
  .cfi_def_cfa_offset 16
  ret
  .cfi_endproc
empty:
  .cfi_startproc
  .cfi_endproc

pers:
  ret

  .section .gcc_except_table,"a"
.Lexcept:
  .byte 0xff, 0xff, 0x01, 0x00

  .data
  .p2align 3
DW.ref.pers:
  .xword pers
