; RUN: opt -mtriple=wasm32-unknown-unknown -passes=expand-variadics --expand-variadics-override=lowering -S < %s | FileCheck %s

; ExpandVariadics rebuilds a variadic call into a call to the non-variadic
; (va_list) equivalent. The 'unwindabort' marker is semantically significant and
; must be preserved on the rebuilt call.

target datalayout = "e-m:e-p:32:32-p10:8:8-p20:8:8-i64:64-n32:64-S128-ni:1:10:20"
target triple = "wasm32-unknown-unknown"

define void @variadic(i32 %0, ...) {
  ret void
}

declare i32 @__gxx_personality_v0(...)

define void @caller(i32 %x) personality ptr @__gxx_personality_v0 {
; CHECK-LABEL: define void @caller(
; CHECK: call unwindabort void @variadic(
  call unwindabort void (i32, ...) @variadic(i32 %x, i32 7)
  ret void
}
