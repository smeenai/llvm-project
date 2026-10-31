; RUN: opt < %s -passes=dse -S | FileCheck %s

;; Folding an allocation and a memset into the allocator's zeroed variant must
;; keep 'unwindabort' on the new allocation call.

declare void @llvm.memset.p0.i64(ptr nocapture, i8, i64, i1) nounwind
declare ptr @customalloc(i64, i64) allockind("alloc,uninitialized") "alloc-family"="customalloc" "alloc-variant-zeroed"="customalloc_zeroed"
declare i32 @__gxx_personality_v0(...)

define ptr @zeroed_variant(i64 %size, i64 %align) personality ptr @__gxx_personality_v0 {
; CHECK-LABEL: @zeroed_variant(
; CHECK-NEXT:    [[CALL:%.*]] = call unwindabort ptr @customalloc_zeroed(i64 [[SIZE:%.*]], i64 [[ALIGN:%.*]])
; CHECK-NEXT:    ret ptr [[CALL]]
  %call = call unwindabort ptr @customalloc(i64 %size, i64 %align)
  call void @llvm.memset.p0.i64(ptr %call, i8 0, i64 %size, i1 false)
  ret ptr %call
}
