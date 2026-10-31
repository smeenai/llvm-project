; RUN: opt < %s -passes='module(coro-early),cgscc(coro-split),simplifycfg' -S | FileCheck %s

; When coro-split lowers a coro.await.suspend whose call is 'unwindabort', the
; rewritten call to the await-suspend wrapper must preserve the marker (so an
; await_suspend that unwinds in a noexcept context still terminates).

%Awaiter = type {}

define void @f() presplitcoroutine personality ptr @__gxx_personality_v0 {
entry:
  %awaiter = alloca %Awaiter
  %id = call token @llvm.coro.id(i32 0, ptr null, ptr null, ptr null)
  %size = call i32 @llvm.coro.size.i32()
  %alloc = call ptr @malloc(i32 %size)
  %hdl = call ptr @llvm.coro.begin(token %id, ptr %alloc)
  %suspend.init = call i8 @llvm.coro.suspend(token none, i1 false)
  switch i8 %suspend.init, label %ret [ i8 0, label %step  i8 1, label %cleanup ]

; CHECK: call unwindabort void @await_suspend_wrapper_void(
step:
  %save = call token @llvm.coro.save(ptr null)
  call unwindabort void @llvm.coro.await.suspend.void(ptr %awaiter, ptr %hdl, ptr @await_suspend_wrapper_void)
  %suspend = call i8 @llvm.coro.suspend(token %save, i1 false)
  switch i8 %suspend, label %ret [ i8 0, label %cleanup  i8 1, label %cleanup ]

cleanup:
  %mem = call ptr @llvm.coro.free(token %id, ptr %hdl)
  call void @free(ptr %mem)
  br label %ret

ret:
  call i1 @llvm.coro.end(ptr %hdl, i1 0, token none)
  ret void
}

declare i32 @__gxx_personality_v0(...)
declare void @await_suspend_wrapper_void(ptr, ptr)
declare ptr @llvm.coro.free(token, ptr)
declare i32 @llvm.coro.size.i32()
declare i8  @llvm.coro.suspend(token, i1)
declare token @llvm.coro.id(i32, ptr, ptr, ptr)
declare ptr @llvm.coro.begin(token, ptr)
declare void @llvm.coro.await.suspend.void(ptr, ptr, ptr)
declare i1 @llvm.coro.end(ptr, i1, token)
declare noalias ptr @malloc(i32)
declare void @free(ptr)
