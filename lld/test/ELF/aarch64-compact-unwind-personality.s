// REQUIRES: aarch64
/// More than 6 personality routines: functions using the 7th and later have
/// personality 7 in the descriptor and a row in the personality side table.
/// The table refers to routines through slots, so p7, which f7's CIE
/// references directly, gets a GOT entry.
// RUN: llvm-mc -filetype=obj -triple=aarch64 %s -o %t.o
// RUN: ld.lld -z compact-unwind %t.o -o %t -Ttext=0x210000 -Tdata=0x230000 \
// RUN:   --section-start=.got=0x250000
// RUN: llvm-readobj --unwind %t | FileCheck %s
// RUN: llvm-objdump -s -j .got %t | FileCheck %s --check-prefix=GOT
// GOT:      Contents of section .got:
// GOT-NEXT: 250000 40002100 00000000

/// A routine referenced as a section plus an offset has no symbol to give a
/// GOT entry.
// RUN: llvm-mc -filetype=obj -triple=aarch64 --defsym LOCAL=1 %s -o %t.local.o
// RUN: not ld.lld -z compact-unwind %t.local.o -o /dev/null 2>&1 | \
// RUN:   FileCheck %s --check-prefix=ERR
// ERR: error: {{.*}}.o:(.eh_frame+0x{{[0-9a-f]+}}): -z compact-unwind needs a symbol for the personality routine, not a symbol plus an addend

// CHECK:      0: 0x230000
// CHECK-NEXT: 1: 0x230008
// CHECK-NEXT: 2: 0x230010
// CHECK-NEXT: 3: 0x230018
// CHECK-NEXT: 4: 0x230020
// CHECK-NEXT: 5: 0x230028
// CHECK-NEXT: 6: 0x230030
// CHECK-NEXT: 7: 0x250000
// CHECK:      Start: 0x210004
// CHECK:      Personality: 1
// CHECK:      Start: 0x210008
// CHECK:      Personality: 2
// CHECK:      Start: 0x21000C
// CHECK:      Personality: 3
// CHECK:      Start: 0x210010
// CHECK:      Personality: 4
// CHECK:      Start: 0x210014
// CHECK:      Personality: 5
// CHECK:      Start: 0x210018
// CHECK:      Personality: 6
// CHECK:      Start: 0x21001C
// CHECK:      Personality: 7
// CHECK:      Start: 0x210020
// CHECK:      Personality: 7
// CHECK:      Kind: Personality (0x2)
// CHECK:      0x21001c: 6
// CHECK-NEXT: 0x210020: 7

  .text
  .globl _start
_start:
  ret

f0:
  .cfi_startproc
  .cfi_personality 0x9b, DW.ref.p0
  .cfi_lsda 0x1b, .Lex0
  ret
  .cfi_endproc

f1:
  .cfi_startproc
  .cfi_personality 0x9b, DW.ref.p1
  .cfi_lsda 0x1b, .Lex1
  ret
  .cfi_endproc

f2:
  .cfi_startproc
  .cfi_personality 0x9b, DW.ref.p2
  .cfi_lsda 0x1b, .Lex2
  ret
  .cfi_endproc

f3:
  .cfi_startproc
  .cfi_personality 0x9b, DW.ref.p3
  .cfi_lsda 0x1b, .Lex3
  ret
  .cfi_endproc

f4:
  .cfi_startproc
  .cfi_personality 0x9b, DW.ref.p4
  .cfi_lsda 0x1b, .Lex4
  ret
  .cfi_endproc

f5:
  .cfi_startproc
  .cfi_personality 0x9b, DW.ref.p5
  .cfi_lsda 0x1b, .Lex5
  ret
  .cfi_endproc

f6:
  .cfi_startproc
  .cfi_personality 0x9b, DW.ref.p6
  .cfi_lsda 0x1b, .Lex6
  ret
  .cfi_endproc

f7:
  .cfi_startproc
  .cfi_personality 0x1b, p7
  .cfi_lsda 0x1b, .Lex7
  ret
  .cfi_endproc

p0:
  ret
p1:
  ret
p2:
  ret
p3:
  ret
p4:
  ret
p5:
  ret
p6:
  ret
  .globl p7
p7:
  ret

.ifdef LOCAL
local:
  .cfi_startproc
  .cfi_personality 0x1b, plocal
  ret
  .cfi_endproc
plocal:
  ret
.endif

  .section .gcc_except_table,"a"
.Lex0:
  .byte 0xff, 0xff, 0x01, 0
.Lex1:
  .byte 0xff, 0xff, 0x01, 1
.Lex2:
  .byte 0xff, 0xff, 0x01, 2
.Lex3:
  .byte 0xff, 0xff, 0x01, 3
.Lex4:
  .byte 0xff, 0xff, 0x01, 4
.Lex5:
  .byte 0xff, 0xff, 0x01, 5
.Lex6:
  .byte 0xff, 0xff, 0x01, 6
.Lex7:
  .byte 0xff, 0xff, 0x01, 7

  .data
  .p2align 3
  .globl DW.ref.p0
  .hidden DW.ref.p0
DW.ref.p0:
  .xword p0
  .globl DW.ref.p1
  .hidden DW.ref.p1
DW.ref.p1:
  .xword p1
  .globl DW.ref.p2
  .hidden DW.ref.p2
DW.ref.p2:
  .xword p2
  .globl DW.ref.p3
  .hidden DW.ref.p3
DW.ref.p3:
  .xword p3
  .globl DW.ref.p4
  .hidden DW.ref.p4
DW.ref.p4:
  .xword p4
  .globl DW.ref.p5
  .hidden DW.ref.p5
DW.ref.p5:
  .xword p5
  .globl DW.ref.p6
  .hidden DW.ref.p6
DW.ref.p6:
  .xword p6
