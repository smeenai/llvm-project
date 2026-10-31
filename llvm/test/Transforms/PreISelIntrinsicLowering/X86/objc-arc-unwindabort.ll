; RUN: opt -mtriple=x86_64-pc-linux-gnu -passes=pre-isel-intrinsic-lowering -S -o - %s | FileCheck %s

; Lowering an ObjC ARC intrinsic to a runtime call rebuilds the CallInst. The
; 'unwindabort' marker is semantically significant and must be carried over to
; the lowered call rather than silently dropped.

declare ptr @llvm.objc.retain(ptr)
declare i32 @__gxx_personality_v0(...)

define ptr @t(ptr %x) personality ptr @__gxx_personality_v0 {
; CHECK-LABEL: @t(
; CHECK: call unwindabort ptr @objc_retain(
  %r = call unwindabort ptr @llvm.objc.retain(ptr %x)
  ret ptr %r
}
