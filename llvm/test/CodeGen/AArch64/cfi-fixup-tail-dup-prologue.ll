; RUN: llc -mtriple=aarch64-linux-gnu -O3 < %s | FileCheck %s

; Shrink-wrapping puts the prologue in a non-entry block, which block placement
; then tail-duplicates, so these functions have two copies of the prologue.

; Both prologue copies execute before the call in %tail, so it is described as
; having a frame: no reset of the CFA to SP+0 at the start of %tail, which would
; describe the return address as live in x30.

; CHECK-LABEL: f:
; CHECK:         stp x30, x19, [sp, #-16]!
; CHECK:         stp x30, x19, [sp, #-16]!
; CHECK:       .LBB0_4:
; CHECK-NOT:     .cfi_def_cfa wsp, 0
; CHECK-NOT:     .cfi_same_value
; CHECK:         bl g

define ptr @f(ptr %a, ptr %b, i64 %n) uwtable {
entry:
  %isnull = icmp eq ptr %a, null
  br i1 %isnull, label %merge, label %load

load:
  %v = load i32, ptr %a, align 8
  %z = zext i32 %v to i64
  br label %merge

merge:
  %x = phi i64 [ %z, %load ], [ 0, %entry ]
  %lt = icmp ult i64 %x, %n
  br i1 %lt, label %load2, label %tail

load2:
  %q = load ptr, ptr %b, align 8
  br label %tail

tail:
  %r = phi ptr [ %q, %load2 ], [ null, %merge ]
  call void @g()
  %res = getelementptr i8, ptr %r, i64 44
  ret ptr %res
}

; The second prologue copy (%load) is entered without a frame but laid out
; after a block that has one, so it needs a reset to the initial state before
; its own CFI; otherwise its .cfi_def_cfa_offset would apply to the x29-based
; CFA that the first copy sets up. %store, reached only after a prologue copy,
; isn't reset.

; CHECK-LABEL: h:
; CHECK:         .cfi_def_cfa w29, 16
; CHECK:       .LBB1_2:
; CHECK-NEXT:    .cfi_def_cfa wsp, 0
; CHECK-NEXT:    .cfi_same_value w30
; CHECK-NEXT:    .cfi_same_value w29
; CHECK:         sub sp, sp, #48
; CHECK:       .LBB1_3:
; CHECK-NOT:     .cfi_
; CHECK:       .LBB1_4:
; CHECK-NEXT:    bl g2

define i32 @h(i16 %flags, ptr %p, i1 %c) uwtable "frame-pointer"="non-leaf" {
entry:
  %buf = alloca [32 x i8], align 8
  %bit = and i16 %flags, 1
  %z = icmp eq i16 %bit, 0
  br i1 %z, label %merge, label %load

load:
  %v = load ptr, ptr %p, align 8
  br label %merge

merge:
  %q = phi ptr [ %v, %load ], [ null, %entry ]
  br i1 %c, label %store, label %call

call:
  %r1 = call i64 @g2()
  br label %exit

store:
  store ptr %q, ptr %buf, align 8
  %r2 = call i64 @g2()
  br label %exit

exit:
  ret i32 0
}

declare void @g()
declare i64 @g2()
