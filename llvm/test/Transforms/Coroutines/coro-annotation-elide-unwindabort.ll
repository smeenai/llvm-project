; RUN: opt < %s -S -passes='cgscc(coro-annotation-elide)' | FileCheck %s

; coro-annotation-elide rebuilds a coro_elide_safe call into a call to the
; coroutine's '.noalloc' variant and inlines it. The 'unwindabort' marker on the
; original call must be preserved on the rebuilt (.noalloc) call; we observe it
; via the inlined coroutine intrinsics, which the inliner marks 'unwindabort'
; only if the elided call carried the marker.

%struct.Task = type { ptr }

declare void @print(i32) nounwind

define fastcc void @callee.resume(ptr dereferenceable(1)) {
  tail call void @print(i32 0)
  ret void
}
define fastcc void @callee.destroy(ptr) {
  tail call void @print(i32 1)
  ret void
}
define fastcc void @callee.cleanup(ptr) {
  tail call void @print(i32 2)
  ret void
}

@callee.resumers = internal constant [3 x ptr] [
  ptr @callee.resume, ptr @callee.destroy, ptr @callee.cleanup]

declare void @alloc(i1) nounwind
declare i32 @__gxx_personality_v0(...)

define ptr @callee(i8 %arg) {
entry:
  %task = alloca %struct.Task, align 8
  %id = call token @llvm.coro.id(i32 0, ptr null, ptr @callee, ptr @callee.resumers)
  %alloc = call i1 @llvm.coro.alloc(token %id)
  %hdl = call ptr @llvm.coro.begin(token %id, ptr null)
  store ptr %hdl, ptr %task
  ret ptr %task
}

define ptr @callee.noalloc(i8 %arg, ptr dereferenceable(32) align(8) %frame) {
 entry:
  %task = alloca %struct.Task, align 8
  %id = call token @llvm.coro.id(i32 0, ptr null, ptr @callee, ptr @callee.resumers)
  %hdl = call ptr @llvm.coro.begin(token %id, ptr null)
  store ptr %hdl, ptr %task
  ret ptr %task
}

; CHECK-LABEL: define ptr @caller()
; The elided+inlined coroutine body's intrinsics carry 'unwindabort', proving
; the rebuilt .noalloc call preserved the marker.
; CHECK: call unwindabort token @llvm.coro.id(
define ptr @caller() #0 personality ptr @__gxx_personality_v0 {
entry:
  %task = call unwindabort ptr @callee(i8 0) #1
  ret ptr %task
}

declare token @llvm.coro.id(i32, ptr, ptr, ptr)
declare ptr @llvm.coro.begin(token, ptr)
declare ptr @llvm.coro.frame()
declare ptr @llvm.coro.subfn.addr(ptr, i8)
declare i1 @llvm.coro.alloc(token)

attributes #0 = { presplitcoroutine }
attributes #1 = { coro_elide_safe }
