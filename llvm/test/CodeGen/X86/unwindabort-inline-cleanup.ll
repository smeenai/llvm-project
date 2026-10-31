; Inlining a callee whose invoke has a catch-only (non-exhaustive) landingpad
; through a 'call unwindabort' must add a 'cleanup' clause, so the resulting
; LSDA action table has a Cleanup action record *before* the Catch action.
; That is what routes a non-matching exception through the landingpad to the
; lowered 'resume unwindabort' (terminate), instead of letting it escape the
; unwindabort scope. This locks in the full opt->llc chain.
; RUN: opt < %s -passes=inline -S | llc -mtriple=x86_64-linux-gnu | FileCheck %s

declare void @maythrow()
declare i32 @__gxx_personality_v0(...)
@_ZTIi = external constant ptr

define internal void @callee_with_catch() personality ptr @__gxx_personality_v0 {
entry:
  invoke void @maythrow() to label %cont unwind label %lpad
cont:
  ret void
lpad:
  %0 = landingpad { ptr, i32 } catch ptr @_ZTIi
  resume { ptr, i32 } %0
}

define void @caller() personality ptr @__gxx_personality_v0 {
entry:
  call unwindabort void @callee_with_catch()
  ret void
}

; CHECK-LABEL: caller:
; CHECK:      GCC_except_table{{[0-9]+}}:
; The call site that can unwind points at the cleanup+catch landingpad.
; CHECK:        # >> Call Site {{[0-9]+}} <<
; CHECK:        .byte 3                         {{.*}}On action: 2
; The action table chains Cleanup -> Catch (cleanup is what makes a
; non-matching exception terminate via the resume unwindabort):
; CHECK:        .byte 0                         # >> Action Record 1 <<
; CHECK-NEXT:                                   #   Cleanup
; CHECK:        .byte 1                         # >> Action Record 2 <<
; CHECK-NEXT:                                   #   Catch TypeInfo 1
