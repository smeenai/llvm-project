// RUN: %clang_cc1 -triple=x86_64-linux-gnu -fexceptions -fcxx-exceptions -emit-llvm -o - %s | \
// RUN:   FileCheck --check-prefixes=CHECK,NOUNWIND %s
// RUN: %clang_cc1 -triple=x86_64-linux-gnu -fexceptions -fcxx-exceptions -funwind-tables=1 -emit-llvm -o - %s | \
// RUN:   FileCheck --check-prefixes=CHECK,SYNCUNWIND %s
// RUN: %clang_cc1 -triple=x86_64-linux-gnu -fexceptions -fcxx-exceptions -funwind-tables=2 -emit-llvm -o - %s | \
// RUN:   FileCheck --check-prefixes=CHECK,ASYNCUNWIND %s

// A potentially-throwing call in a noexcept function is now emitted as a
// 'call unwindabort' instead of an invoke to a terminate landingpad, so
// __clang_call_terminate is no longer generated.
void caller(void callback()) noexcept { callback(); }

// CHECK-LABEL: define {{.*}}void @_Z6callerPFvvE(
// CHECK:         call unwindabort void %{{.*}}()
// CHECK-NOT:   @__clang_call_terminate

// The uwtable variants still differ on the caller itself.
// NOUNWIND-NOT:   uwtable
// SYNCUNWIND:     uwtable(sync)
// ASYNCUNWIND:    uwtable{{[ )]}}
