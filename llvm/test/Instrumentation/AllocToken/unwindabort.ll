; RUN: opt < %s -passes=inferattrs,alloc-token -S | FileCheck %s

;; Replacing a 'call unwindabort' to an allocation function with its token
;; variant must keep the marker: operator new may throw.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"

declare ptr @_Znwm(i64)
declare i32 @__gxx_personality_v0(...)

define ptr @test_new() sanitize_alloc_token personality ptr @__gxx_personality_v0 {
; CHECK-LABEL: @test_new(
; CHECK: call unwindabort ptr @__alloc_token__Znwm(i64 10, i64 {{[0-9]+}})
  %p = call unwindabort ptr @_Znwm(i64 10), !alloc_token !0
  ret ptr %p
}

!0 = !{!"int", i1 0}
