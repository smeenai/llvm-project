// REQUIRES: x86
// RUN: llvm-mc -filetype=obj -triple=x86_64 %s -o %t.o
// RUN: not ld.lld -z compact-unwind %t.o -o /dev/null 2>&1 | FileCheck %s
// CHECK: error: -z compact-unwind only supported on AArch64



  .globl _start
_start:
  ret
