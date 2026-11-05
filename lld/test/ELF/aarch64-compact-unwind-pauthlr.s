// REQUIRES: aarch64
/// PAuth_LR: authenticating the return address needs the signing instruction,
/// which the prologue's lead-in (async) or N (sync) locates, or else the
/// PAuth_LR side table.
// RUN: llvm-mc -filetype=obj -triple=aarch64 %s -o %t.o
// RUN: ld.lld -z compact-unwind %t.o -o %t -Ttext=0x210000
// RUN: llvm-readobj --unwind %t | FileCheck %s --check-prefix=ASYNC
// RUN: ld.lld -z compact-unwind=sync %t.o -o %t.sync -Ttext=0x210000
// RUN: llvm-readobj --unwind %t.sync | FileCheck %s --check-prefix=SYNC

// ASYNC:      Start: 0x210000
// ASYNC-NEXT: End: 0x21002C
// ASYNC:      Mode: Escape
// ASYNC:      0x210000: CFA=reg31
// ASYNC-NEXT: 0x210008: CFA=reg31: reg34=2
// ASYNC-NEXT: 0x21000c: CFA=reg31+16: reg34=2
// ASYNC-NEXT: 0x210010: CFA=reg29+16: reg29=[CFA-16], reg30=[CFA-8], reg34=2
// ASYNC-NEXT: 0x210014: CFA=reg31+16: reg29=[CFA-16], reg30=[CFA-8], reg34=2
// ASYNC-NEXT: 0x210018: CFA=reg31: reg34=2
// ASYNC-NEXT: 0x210028: CFA=reg31
// ASYNC:      Start: 0x21002C
// ASYNC-NEXT: End: 0x210054
// ASYNC:      Mode: Escape
// ASYNC:      0x21002c: CFA=reg31
// ASYNC-NEXT: 0x210034: CFA=reg31: reg34=2
// ASYNC-NEXT: 0x210038: CFA=reg31+16: reg30=[CFA-16], reg34=2
// ASYNC-NEXT: 0x210040: CFA=reg31: reg34=2
// ASYNC-NEXT: 0x210050: CFA=reg31

// SYNC:      SyncOnly (0x1)
// SYNC:      Start: 0x210000
// SYNC-NEXT: End: 0x21002C
// SYNC:      Mode: Escape
// SYNC:      0x210000: CFA=reg31
// SYNC-NEXT: 0x210008: CFA=reg31: reg34=2
// SYNC-NEXT: 0x21000c: CFA=reg31+16: reg34=2
// SYNC-NEXT: 0x210010: CFA=reg29+16: reg29=[CFA-16], reg30=[CFA-8], reg34=2
// SYNC-NEXT: 0x210014: CFA=reg31+16: reg29=[CFA-16], reg30=[CFA-8], reg34=2
// SYNC-NEXT: 0x210018: CFA=reg31: reg34=2
// SYNC-NEXT: 0x210028: CFA=reg31
// SYNC:      Start: 0x21002C
// SYNC-NEXT: End: 0x210054
// SYNC:      Mode: Escape
// SYNC:      0x21002c: CFA=reg31
// SYNC-NEXT: 0x210034: CFA=reg31: reg34=2
// SYNC-NEXT: 0x210038: CFA=reg31+16: reg30=[CFA-16], reg34=2
// SYNC-NEXT: 0x210040: CFA=reg31: reg34=2
// SYNC-NEXT: 0x210050: CFA=reg31

  .text
  .globl _start
_start:
  .cfi_startproc
  hint #39 // pacm
.Lsign:
  hint #25 // paciasp
  .cfi_set_ra_state 2, .Lsign
  stp x29, x30, [sp, #-16]!
  .cfi_def_cfa_offset 16
  mov x29, sp
  .cfi_def_cfa w29, 16
  .cfi_offset w30, -8
  .cfi_offset w29, -16
  bl _start
  .cfi_def_cfa wsp, 16
  ldp x29, x30, [sp], #16
  .cfi_def_cfa_offset 0
  .cfi_restore w30
  .cfi_restore w29
  adrp x16, .Lsign
  add x16, x16, :lo12:.Lsign
  hint #39 // pacm
  hint #29 // autiasp
  .cfi_set_ra_state 0, 0
  ret
  .cfi_endproc

/// Without a frame pointer, N holds the frame size, so sync tables locate the
/// signing instruction with a side table row.
sp_frame:
  .cfi_startproc
  hint #39 // pacm
.Lsign2:
  hint #25 // paciasp
  .cfi_set_ra_state 2, .Lsign2
  str x30, [sp, #-16]!
  .cfi_def_cfa_offset 16
  .cfi_offset w30, -16
  bl _start
  ldr x30, [sp], #16
  .cfi_def_cfa_offset 0
  .cfi_restore w30
  adrp x16, .Lsign2
  add x16, x16, :lo12:.Lsign2
  hint #39 // pacm
  hint #29 // autiasp
  .cfi_set_ra_state 0, 0
  ret
  .cfi_endproc
