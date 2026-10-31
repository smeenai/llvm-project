; RUN: opt -passes=mergefunc -S < %s | FileCheck %s

; FunctionComparator must treat two otherwise-identical functions that differ
; only in a call's 'unwindabort' marker as DIFFERENT, so MergeFunctions does not
; merge them and silently drop the marker from one.

declare void @ext()
declare i32 @__gxx_personality_v0(...)
declare i32 @__CxxFrameHandler3(...)

; CHECK-LABEL: define void @a(
; CHECK: call unwindabort void @ext()
define void @a(i32 %x) personality ptr @__gxx_personality_v0 {
  %c = icmp eq i32 %x, 0
  br i1 %c, label %t, label %f
t:
  call unwindabort void @ext()
  br label %f
f:
  ret void
}

; CHECK-LABEL: define void @b(
; CHECK: call void @ext()
; CHECK-NOT: unwindabort
define void @b(i32 %x) personality ptr @__gxx_personality_v0 {
  %c = icmp eq i32 %x, 0
  br i1 %c, label %t, label %f
t:
  call void @ext()
  br label %f
f:
  ret void
}

; Same for 'resume': two functions differing only in 'resume unwindabort' vs a
; plain 'resume' must not be merged.
; CHECK-LABEL: define void @r_abort(
; CHECK: resume unwindabort
define void @r_abort() personality ptr @__gxx_personality_v0 {
  invoke void @ext() to label %ok unwind label %lp
ok:
  ret void
lp:
  %e = landingpad { ptr, i32 } cleanup
  resume unwindabort { ptr, i32 } %e
}

; CHECK-LABEL: define void @r_plain(
; CHECK: resume { ptr, i32 }
; CHECK-NOT: resume unwindabort
define void @r_plain() personality ptr @__gxx_personality_v0 {
  invoke void @ext() to label %ok unwind label %lp
ok:
  ret void
lp:
  %e = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %e
}

; And for 'catchswitch': 'unwindabort' vs 'unwind to caller' must not be merged.
; CHECK-LABEL: define void @cs_abort(
; CHECK: catchswitch within none [label %{{.*}}] unwindabort
define void @cs_abort() personality ptr @__CxxFrameHandler3 {
  invoke void @ext() to label %ok unwind label %cs
ok:
  ret void
cs:
  %c = catchswitch within none [label %catch] unwindabort
catch:
  %p = catchpad within %c [ptr null, i32 64, ptr null]
  catchret from %p to label %ok
}

; CHECK-LABEL: define void @cs_caller(
; CHECK: catchswitch within none [label %{{.*}}] unwind to caller
; CHECK-NOT: unwindabort
define void @cs_caller() personality ptr @__CxxFrameHandler3 {
  invoke void @ext() to label %ok unwind label %cs
ok:
  ret void
cs:
  %c = catchswitch within none [label %catch] unwind to caller
catch:
  %p = catchpad within %c [ptr null, i32 64, ptr null]
  catchret from %p to label %ok
}
