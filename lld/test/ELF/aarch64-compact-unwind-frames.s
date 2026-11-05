// REQUIRES: aarch64
/// Frame shapes that descriptors describe: LLVM and GCC save area conventions,
/// return address signing, the shadow call stack, and GCC saving registers one
/// at a time.

// RUN: llvm-mc -filetype=obj -triple=aarch64 %s -o %t.o
// RUN: ld.lld -z compact-unwind %t.o -o %t -Ttext=0x210000
// RUN: llvm-readobj --unwind %t | FileCheck %s --check-prefix=ASYNC
// RUN: ld.lld -z compact-unwind=sync %t.o -o %t.sync -Ttext=0x210000
// RUN: llvm-readobj --unwind %t.sync | FileCheck %s --check-prefix=SYNC

// ASYNC:      Start: 0x210000
// ASYNC-NEXT: End: 0x21002C
// ASYNC:      Mode: FP
// ASYNC-NEXT: Convention: LLVM
// ASYNC-NEXT: Registers: x19, x20, x29, x30, d8
// ASYNC-NEXT: FrameRecord: Yes
// ASYNC-NEXT: O: 0
// ASYNC-NEXT: N: 3
// ASYNC-NEXT: RA: A
// ASYNC-NEXT: Prologue: apos=1 PS=5
// ASYNC-NEXT: Epilogue: window=3 tail=2
// ASYNC:      0x210000: CFA=reg31
// ASYNC-NEXT: 0x210004: CFA=reg31: reg34=1
// ASYNC-NEXT: 0x210008: CFA=reg31+48: reg34=1
// ASYNC-NEXT: 0x210014: CFA=reg29+32: reg19=[CFA-8], reg20=[CFA-16], reg29=[CFA-32], reg30=[CFA-24], reg34=1, reg72=[CFA-48]
// ASYNC-NEXT: 0x210018: CFA=reg31+48: reg19=[CFA-8], reg20=[CFA-16], reg29=[CFA-32], reg30=[CFA-24], reg34=1, reg72=[CFA-48]
// ASYNC-NEXT: 0x210024: CFA=reg31: reg34=1
// ASYNC-NEXT: 0x210028: CFA=reg31
// ASYNC:      Start: 0x21002C
// ASYNC-NEXT: End: 0x210044
// ASYNC:      Mode: SP
// ASYNC-NEXT: Convention: LLVM
// ASYNC-NEXT: Registers: x19, x30, d8
// ASYNC-NEXT: O: 0
// ASYNC-NEXT: N: 2
// ASYNC-NEXT: Prologue: apos=0 PS=2
// ASYNC-NEXT: Epilogue: window=0 tail=1
// ASYNC:      0x21002c: CFA=reg31
// ASYNC-NEXT: 0x210030: CFA=reg31+32
// ASYNC-NEXT: 0x210034: CFA=reg31+32: reg19=[CFA-8], reg30=[CFA-16], reg72=[CFA-32]
// ASYNC-NEXT: 0x210040: CFA=reg31
// ASYNC:      Start: 0x210044
// ASYNC-NEXT: End: 0x210048
// ASYNC:      Mode: NULL
// ASYNC-NEXT: Convention: LLVM
// ASYNC-NEXT: N: 0
// ASYNC:      0x210044: CFA=reg31
// ASYNC:      Start: 0x210048
// ASYNC-NEXT: End: 0x21004C
// ASYNC:      Mode: NULL
// ASYNC-NEXT: Convention: LLVM
// ASYNC-NEXT: ShadowCallStack: Yes
// ASYNC-NEXT: N: 0
// ASYNC:      0x210048: CFA=reg31: reg18=DW_OP_breg18 -8
// ASYNC:      Start: 0x21004C
// ASYNC-NEXT: End: 0x210050
// ASYNC:      Mode: SP
// ASYNC-NEXT: Convention: LLVM
// ASYNC-NEXT: ShadowCallStack: Yes
// ASYNC-NEXT: Registers:
// ASYNC-NEXT: O: 0
// ASYNC-NEXT: N: 1
// ASYNC:      0x21004c: CFA=reg31+16: reg18=DW_OP_breg18 -8
// ASYNC:      Start: 0x210050
// ASYNC-NEXT: End: 0x210054
// ASYNC:      Mode: FP
// ASYNC-NEXT: Convention: LLVM
// ASYNC-NEXT: ShadowCallStack: Yes
// ASYNC-NEXT: Registers: x29, x30
// ASYNC-NEXT: FrameRecord: Yes
// ASYNC-NEXT: O: 0
// ASYNC-NEXT: N: 0
// ASYNC:      0x210050: CFA=reg29+16: reg18=DW_OP_breg18 -8, reg29=[CFA-16], reg30=[CFA-8]
// ASYNC:      Start: 0x210054
// ASYNC-NEXT: End: 0x210058
// ASYNC:      Mode: SP
// ASYNC-NEXT: Convention: LLVM
// ASYNC-NEXT: ShadowCallStack: Yes
// ASYNC-NEXT: Registers: x29, x30
// ASYNC-NEXT: FrameRecord: Yes
// ASYNC-NEXT: O: 0
// ASYNC-NEXT: N: 1
// ASYNC:      0x210054: CFA=reg31+16: reg18=DW_OP_breg18 -8, reg29=[CFA-16], reg30=[CFA-8]
// ASYNC:      Start: 0x210058
// ASYNC-NEXT: End: 0x21005C
// ASYNC:      Mode: NULL
// ASYNC-NEXT: Convention: LLVM
// ASYNC-NEXT: ShadowCallStack: Yes
// ASYNC-NEXT: N: 0
// ASYNC:      0x210058: CFA=reg31: reg18=DW_OP_breg18 -8
// ASYNC:      Start: 0x21005C
// ASYNC-NEXT: End: 0x210060
// ASYNC:      Mode: NULL
// ASYNC-NEXT: Convention: LLVM
// ASYNC-NEXT: N: 0
// ASYNC:      0x21005c: CFA=reg31
// ASYNC:      Start: 0x210060
// ASYNC-NEXT: End: 0x21007C
// ASYNC:      Mode: SP
// ASYNC-NEXT: Convention: GCC
// ASYNC-NEXT: Registers: x29, x30
// ASYNC-NEXT: FrameRecord: Yes
// ASYNC-NEXT: O: 1
// ASYNC-NEXT: N: 5
// ASYNC-NEXT: Prologue: apos=0 PS=2
// ASYNC-NEXT: Epilogue: window=0 tail=1
// ASYNC:      0x210060: CFA=reg31
// ASYNC-NEXT: 0x210064: CFA=reg31+80
// ASYNC-NEXT: 0x210068: CFA=reg31+80: reg29=[CFA-64], reg30=[CFA-56]
// ASYNC-NEXT: 0x210078: CFA=reg31
// ASYNC:      Start: 0x21007C
// ASYNC-NEXT: End: 0x2100A8
// ASYNC:      Mode: Escape
// ASYNC:      0x21007c: CFA=reg31
// ASYNC-NEXT: 0x210080: CFA=reg31+48: reg29=[CFA-48], reg30=[CFA-40]
// ASYNC-NEXT: 0x210088: CFA=reg31+48: reg29=[CFA-48], reg30=[CFA-40], reg79=[CFA-16]
// ASYNC-NEXT: 0x210090: CFA=reg31+48: reg19=[CFA-32], reg20=[CFA-24], reg29=[CFA-48], reg30=[CFA-40], reg79=[CFA-16]
// ASYNC-NEXT: 0x2100a4: CFA=reg31
// ASYNC:      Start: 0x2100A8
// ASYNC-NEXT: End: 0x2100AC
// ASYNC:      Mode: NULL
// ASYNC-NEXT: Convention: LLVM
// ASYNC-NEXT: N: 0
// ASYNC:      0x2100a8: CFA=reg31
// ASYNC:      Start: 0x2100AC
// ASYNC-NEXT: End: 0x2100B0
// ASYNC:      Mode: SP
// ASYNC-NEXT: Convention: LLVM
// ASYNC-NEXT: Registers:
// ASYNC-NEXT: O: 0
// ASYNC-NEXT: N: 25
// ASYNC:      0x2100ac: CFA=reg31+400
// ASYNC:      Start: 0x2100B0
// ASYNC-NEXT: End: 0x2100B8
// ASYNC:      Mode: SP
// ASYNC-NEXT: Convention: LLVM
// ASYNC-NEXT: Registers: x29, x30
// ASYNC-NEXT: FrameRecord: Yes
// ASYNC-NEXT: O: 2
// ASYNC-NEXT: N: 25
// ASYNC:      0x2100b0: CFA=reg31+400: reg29=[CFA-48], reg30=[CFA-40]
// ASYNC:      Start: 0x2100B8
// ASYNC-NEXT: End: 0x2100BC
// ASYNC:      Mode: SP
// ASYNC-NEXT: Convention: GCC CFA-anchored
// ASYNC-NEXT: Registers: x19, x20, x29, x30
// ASYNC-NEXT: FrameRecord: Yes
// ASYNC-NEXT: O: 1
// ASYNC-NEXT: N: 25
// ASYNC:      0x2100b8: CFA=reg31+400: reg19=[CFA-32], reg20=[CFA-24], reg29=[CFA-48], reg30=[CFA-40]
// ASYNC:      Start: 0x2100BC
// ASYNC-NEXT: End: 0x2100D0
// ASYNC:      Mode: SP
// ASYNC-NEXT: Convention: GCC CFA-anchored
// ASYNC-NEXT: Registers: x19, x20, x21, x29, x30
// ASYNC-NEXT: FrameRecord: Yes
// ASYNC-NEXT: O: 0
// ASYNC-NEXT: N: 25
// ASYNC:      0x2100bc: CFA=reg31+400: reg19=[CFA-32], reg20=[CFA-24], reg21=[CFA-16], reg29=[CFA-48], reg30=[CFA-40]
// ASYNC:      Start: 0x2100D0
// ASYNC-NEXT: End: 0x2100DC
// ASYNC:      Mode: NULL
// ASYNC-NEXT: Convention: LLVM
// ASYNC-NEXT: N: 0
// ASYNC:      0x2100d0: CFA=reg31

// SYNC:      SyncOnly (0x1)
// SYNC:      Start: 0x210000
// SYNC-NEXT: End: 0x21002C
// SYNC:      Mode: FP
// SYNC-NEXT: Convention: LLVM
// SYNC-NEXT: Registers: x19, x20, x29, x30, d8
// SYNC-NEXT: FrameRecord: Yes
// SYNC-NEXT: O: 0
// SYNC-NEXT: N: 0
// SYNC-NEXT: RA: A
// SYNC:      0x210000: CFA=reg29+32: reg19=[CFA-8], reg20=[CFA-16], reg29=[CFA-32], reg30=[CFA-24], reg34=1, reg72=[CFA-48]
// SYNC:      Start: 0x21002C
// SYNC-NEXT: End: 0x210044
// SYNC:      Mode: SP
// SYNC-NEXT: Convention: LLVM
// SYNC-NEXT: Registers: x19, x30, d8
// SYNC-NEXT: O: 0
// SYNC-NEXT: N: 2
// SYNC:      0x21002c: CFA=reg31+32: reg19=[CFA-8], reg30=[CFA-16], reg72=[CFA-32]
// SYNC:      Start: 0x210044
// SYNC-NEXT: End: 0x210060
// SYNC:      Mode: FP
// SYNC-NEXT: Convention: LLVM
// SYNC-NEXT: ShadowCallStack: Yes
// SYNC-NEXT: Registers: x29, x30
// SYNC-NEXT: FrameRecord: Yes
// SYNC-NEXT: O: 0
// SYNC-NEXT: N: 0
// SYNC:      0x210044: CFA=reg29+16: reg18=DW_OP_breg18 -8, reg29=[CFA-16], reg30=[CFA-8]
// SYNC:      Start: 0x210060
// SYNC-NEXT: End: 0x21007C
// SYNC:      Mode: SP
// SYNC-NEXT: Convention: GCC
// SYNC-NEXT: Registers: x29, x30
// SYNC-NEXT: FrameRecord: Yes
// SYNC-NEXT: O: 1
// SYNC-NEXT: N: 5
// SYNC:      0x210060: CFA=reg31+80: reg29=[CFA-64], reg30=[CFA-56]
// SYNC:      Start: 0x21007C
// SYNC-NEXT: End: 0x2100A8
// SYNC:      Mode: SP
// SYNC-NEXT: Convention: GCC
// SYNC-NEXT: Registers: x19, x20, x29, x30, d15
// SYNC-NEXT: FrameRecord: Yes
// SYNC-NEXT: O: 0
// SYNC-NEXT: N: 3
// SYNC:      0x21007c: CFA=reg31+48: reg19=[CFA-32], reg20=[CFA-24], reg29=[CFA-48], reg30=[CFA-40], reg79=[CFA-16]
// SYNC:      Start: 0x2100A8
// SYNC-NEXT: End: 0x2100D4
// SYNC:      Mode: SP
// SYNC-NEXT: Convention: GCC CFA-anchored
// SYNC-NEXT: Registers: x19, x20, x21, x29, x30
// SYNC-NEXT: FrameRecord: Yes
// SYNC-NEXT: O: 0
// SYNC-NEXT: N: 25
// SYNC:      0x2100a8: CFA=reg31+400: reg19=[CFA-32], reg20=[CFA-24], reg21=[CFA-16], reg29=[CFA-48], reg30=[CFA-40]
// SYNC:      Start: 0x2100D4
// SYNC-NEXT: End: 0x2100DC
// SYNC:      Mode: NULL
// SYNC-NEXT: Convention: LLVM
// SYNC-NEXT: N: 0
// SYNC:      0x2100d4: CFA=reg31

  .text
  .globl _start
  .p2align 2
/// Clang, frame pointer, pac-ret: x19, x20 and d8, with a hole above d8.
_start:
  .cfi_startproc
  paciasp
  .cfi_negate_ra_state
  str d8, [sp, #-48]!
  .cfi_def_cfa_offset 48
  stp x29, x30, [sp, #16]
  stp x20, x19, [sp, #32]
  add x29, sp, #16
  .cfi_def_cfa w29, 32
  .cfi_offset w19, -8
  .cfi_offset w20, -16
  .cfi_offset w30, -24
  .cfi_offset w29, -32
  .cfi_offset b8, -48
  bl nofp
  .cfi_def_cfa wsp, 48
  ldp x20, x19, [sp, #32]
  ldp x29, x30, [sp, #16]
  ldr d8, [sp], #48
  .cfi_def_cfa_offset 0
  .cfi_restore w19
  .cfi_restore w20
  .cfi_restore w30
  .cfi_restore w29
  .cfi_restore b8
  autiasp
  .cfi_negate_ra_state
  ret
  .cfi_endproc

/// Clang -fomit-frame-pointer: x30 is paired with x19, and the hole is above
/// d8.
  .p2align 2
nofp:
  .cfi_startproc
  str d8, [sp, #-32]!
  .cfi_def_cfa_offset 32
  stp x30, x19, [sp, #16]
  .cfi_offset w19, -8
  .cfi_offset w30, -16
  .cfi_offset b8, -32
  bl scs
  ldp x30, x19, [sp, #16]
  ldr d8, [sp], #32
  .cfi_def_cfa_offset 0
  .cfi_restore w19
  .cfi_restore w30
  .cfi_restore b8
  ret
  .cfi_endproc

/// Clang's shadow call stack: x18 is the caller's x18 + 8 in the body.
  .p2align 2
scs:
  .cfi_startproc
  str x30, [x18], #8
  .cfi_escape 0x16, 0x12, 0x02, 0x82, 0x78 // DW_CFA_val_expression x18, DW_OP_breg18 -8
  stp x29, x30, [sp, #-16]!
  .cfi_def_cfa_offset 16
  mov x29, sp
  .cfi_def_cfa w29, 16
  .cfi_offset w30, -8
  .cfi_offset w29, -16
  bl gcc_outgoing
  .cfi_def_cfa wsp, 16
  ldp x29, x30, [sp], #16
  .cfi_def_cfa_offset 0
  .cfi_restore w30
  .cfi_restore w29
  ldr x30, [x18, #-8]!
  .cfi_restore w18
  ret
  .cfi_endproc

/// GCC, sp-anchored: 16 bytes of outgoing arguments below the frame record.
  .p2align 2
gcc_outgoing:
  .cfi_startproc
  sub sp, sp, #80
  .cfi_def_cfa_offset 80
  stp x29, x30, [sp, #16]
  .cfi_offset 29, -64
  .cfi_offset 30, -56
  add x29, sp, #16
  bl gcc_partial
  ldp x29, x30, [sp, #16]
  add sp, sp, #80
  .cfi_restore 29
  .cfi_restore 30
  .cfi_def_cfa_offset 0
  ret
  .cfi_endproc

/// GCC records each save: d15 is saved and reused before x19 and x20 are
/// saved.
  .p2align 2
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
  bl gcc_protector
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

/// GCC with a stack protector: the save area is at the top of the frame.
  .p2align 2
gcc_protector:
  .cfi_startproc
  sub sp, sp, #400
  .cfi_def_cfa_offset 400
  stp x29, x30, [sp, #352]
  .cfi_offset 29, -48
  .cfi_offset 30, -40
  add x29, sp, #352
  stp x19, x20, [sp, #368]
  .cfi_offset 19, -32
  .cfi_offset 20, -24
  str x21, [sp, #384]
  .cfi_offset 21, -16
  bl leaf1
  ldp x19, x20, [sp, #368]
  ldr x21, [sp, #384]
  ldp x29, x30, [sp, #352]
  add sp, sp, #400
  .cfi_restore 29
  .cfi_restore 30
  .cfi_restore 21
  .cfi_restore 19
  .cfi_restore 20
  .cfi_def_cfa_offset 0
  ret
  .cfi_endproc

/// Adjacent leaf functions with the same descriptor share a region.
  .p2align 2
leaf1:
  .cfi_startproc
  ret
  .cfi_endproc
leaf2:
  .cfi_startproc
  ret
  .cfi_endproc
