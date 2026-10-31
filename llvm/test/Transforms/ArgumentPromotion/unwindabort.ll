; RUN: opt -passes=argpromotion -S < %s | FileCheck %s

; ArgumentPromotion rebuilds call sites with promoted arguments. The
; 'unwindabort' marker must be preserved on the rebuilt call.

declare void @ext()
declare i32 @__gxx_personality_v0(...)

define internal void @callee(ptr %p) {
  %v = load i32, ptr %p
  call void @ext()
  ret void
}

define void @caller(ptr %p) personality ptr @__gxx_personality_v0 {
; CHECK-LABEL: define void @caller(
; CHECK: call unwindabort void @callee(
  call unwindabort void @callee(ptr %p)
  ret void
}
