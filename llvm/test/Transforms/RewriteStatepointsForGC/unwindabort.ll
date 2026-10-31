; RUN: opt -passes=rewrite-statepoints-for-gc -S < %s | FileCheck %s

; RewriteStatepointsForGC rebuilds calls as gc.statepoints. The 'unwindabort'
; marker is semantically significant (the backend statepoint lowering honors it),
; so it must survive the rebuild rather than being silently dropped.

declare void @g()
declare i32 @__gxx_personality_v0(...)

define void @f() gc "statepoint-example" personality ptr @__gxx_personality_v0 {
; CHECK-LABEL: @f(
; CHECK: call unwindabort token {{.*}}@llvm.experimental.gc.statepoint
  call unwindabort void @g()
  ret void
}
