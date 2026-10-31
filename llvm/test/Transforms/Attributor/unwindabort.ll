; RUN: opt -passes=attributor -attributor-manifest-internal -S < %s | FileCheck %s

; Attributor rewrites function signatures (here, dropping the dead argument) and
; rebuilds the call sites via CallSiteReplacementCreator. The 'unwindabort'
; marker is semantically significant, not an attribute: it must survive the
; rebuild, or an exception that should terminate would instead propagate out of
; the rewritten call site.

declare void @ext()
declare i32 @__gxx_personality_v0(...)

define internal void @callee(i32 %dead) {
  call void @ext()
  ret void
}

define void @caller() personality ptr @__gxx_personality_v0 {
; CHECK-LABEL: define void @caller(
; CHECK: call unwindabort void @callee
  call unwindabort void @callee(i32 0)
  ret void
}
