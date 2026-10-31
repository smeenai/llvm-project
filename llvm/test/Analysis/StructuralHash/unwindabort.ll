; RUN: opt -passes='print<structural-hash><detailed>' -disable-output %s 2>&1 | FileCheck %s

; 'unwindabort' is semantically significant on call/resume/catchswitch but is not
; implied by the opcode or operands. The detailed structural hash must include it
; so that functions differing ONLY in the marker get DIFFERENT hashes -- otherwise
; structural-hash-based mergers (e.g. GlobalMergeFunctions, which merges without a
; FunctionComparator gate) could merge them and silently drop the marker.

declare void @ext()
declare i32 @__gxx_personality_v0(...)

; CHECK: Function f_call_abort Hash: [[CALL:[0-9a-f]+]]
define void @f_call_abort() personality ptr @__gxx_personality_v0 {
  call unwindabort void @ext()
  ret void
}

; The plain-call twin must NOT share the hash of the unwindabort one.
; CHECK-NOT: Function f_call_plain Hash: [[CALL]]
define void @f_call_plain() personality ptr @__gxx_personality_v0 {
  call void @ext()
  ret void
}

; CHECK: Function f_resume_abort Hash: [[RES:[0-9a-f]+]]
define void @f_resume_abort() personality ptr @__gxx_personality_v0 {
  invoke void @ext() to label %ok unwind label %lp
ok:
  ret void
lp:
  %e = landingpad { ptr, i32 } cleanup
  resume unwindabort { ptr, i32 } %e
}

; CHECK-NOT: Function f_resume_plain Hash: [[RES]]
define void @f_resume_plain() personality ptr @__gxx_personality_v0 {
  invoke void @ext() to label %ok unwind label %lp
ok:
  ret void
lp:
  %e = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %e
}
