// RUN: llvm-mc -filetype=obj -triple=x86_64-pc-linux-gnu -o %t.o -omit-empty-fdes %s
// RUN: llvm-dwarfdump --eh-frame %t.o | FileCheck %s
// CHECK-NOT: FDE
f:
	.cfi_startproc
	ret
	.cfi_endproc
