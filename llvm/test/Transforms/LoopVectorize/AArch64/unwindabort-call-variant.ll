; RUN: opt -passes=loop-vectorize -force-vector-interleave=1 -S < %s | FileCheck %s

;; Widening a 'call unwindabort' into a call to a vector variant must keep the
;; marker on the vector call.

; CHECK-LABEL: define void @widen(
; CHECK:       vector.body:
; CHECK:         call unwindabort <vscale x 2 x i64> @foo_vector_nomask(
; CHECK:       loop:
; CHECK:         call unwindabort i64 @foo(

target triple = "aarch64-unknown-linux-gnu"

declare i64 @foo(i64)
declare <vscale x 2 x i64> @foo_vector_nomask(<vscale x 2 x i64>)
declare i32 @__gxx_personality_v0(...)

define void @widen(ptr noalias %a, ptr noalias %b, i64 %n) #0 personality ptr @__gxx_personality_v0 {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr i64, ptr %a, i64 %i
  %x = load i64, ptr %pa
  %y = call unwindabort i64 @foo(i64 %x) #1
  %pb = getelementptr i64, ptr %b, i64 %i
  store i64 %y, ptr %pb
  %i.next = add nuw nsw i64 %i, 1
  %done = icmp eq i64 %i.next, 1024
  br i1 %done, label %exit, label %loop
exit:
  ret void
}

attributes #0 = { "target-features"="+sve" }
attributes #1 = { memory(none) "vector-function-abi-variant"="_ZGVsNxv_foo(foo_vector_nomask)" }
