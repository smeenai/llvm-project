; RUN: opt -passes=simplifycfg -S < %s | FileCheck %s

; SimplifyCFG's hoist/sink of common code uses Instruction::isIdenticalToWhenDefined
; (-> hasSameSpecialState). A 'call unwindabort' and a plain 'call' to the same
; target with the same operands are NOT interchangeable, so they must not be
; sunk/hoisted into a single call -- doing so silently changes one path's
; exception behavior (terminate vs propagate).

declare void @g()
declare i32 @__gxx_personality_v0(...)

; Sink: the two calls end predecessors of a common successor. They differ only in
; 'unwindabort', so both must remain.
define void @no_sink(i1 %c) personality ptr @__gxx_personality_v0 {
; CHECK-LABEL: @no_sink(
; CHECK: call unwindabort void @g()
; CHECK: call void @g()
entry:
  br i1 %c, label %a, label %b
a:
  call unwindabort void @g()
  br label %j
b:
  call void @g()
  br label %j
j:
  ret void
}

; Hoist: the two calls start the successors of a conditional branch. Same rule.
define void @no_hoist(i1 %c) personality ptr @__gxx_personality_v0 {
; CHECK-LABEL: @no_hoist(
; CHECK: call unwindabort void @g()
; CHECK: call void @g()
entry:
  br i1 %c, label %a, label %b
a:
  call unwindabort void @g()
  ret void
b:
  call void @g()
  ret void
}
