; RUN: opt -passes=deadargelim -S < %s | FileCheck %s

; DeadArgumentElimination rebuilds call sites with a new signature. The
; 'unwindabort' marker is semantically significant and must be preserved on the
; rebuilt call.

declare void @ext()
declare i32 @__gxx_personality_v0(...)

define internal void @callee(i32 %dead) {
  call void @ext()
  ret void
}

define void @caller() personality ptr @__gxx_personality_v0 {
; CHECK-LABEL: define void @caller(
; CHECK: call unwindabort void @callee()
  call unwindabort void @callee(i32 0)
  ret void
}
