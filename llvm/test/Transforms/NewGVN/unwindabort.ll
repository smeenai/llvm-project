; RUN: opt -passes=newgvn -S < %s | FileCheck %s

; GVN value-numbers readnone (and readonly) calls. A 'call unwindabort' is not
; interchangeable with an otherwise-identical plain call, so the two must not be
; value-numbered together -- doing so would eliminate one and drop (or wrongly
; add) the marker.

declare i32 @g() memory(none)
declare i32 @__gxx_personality_v0(...)

define i32 @t() personality ptr @__gxx_personality_v0 {
; CHECK-LABEL: @t(
; CHECK: %a = call unwindabort i32 @g()
; CHECK: %b = call i32 @g()
; CHECK: add i32 %a, %b
  %a = call unwindabort i32 @g()
  %b = call i32 @g()
  %r = add i32 %a, %b
  ret i32 %r
}
