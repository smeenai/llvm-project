// RUN: %clang_cc1 %s -triple=x86_64-apple-darwin10 -emit-llvm -o - -fcxx-exceptions -fexceptions -std=c++11 | FileCheck %s

//   In a noexcept function, potentially-unwinding calls are emitted as
//   'call unwindabort' (which terminates without running cleanups), instead of
//   invokes to a terminate landingpad calling __clang_call_terminate.
namespace test0 {
  void foo();

  struct A {
    A();
    ~A();
  };

  void test() noexcept {
    A a;
    foo();
  }
}

// CHECK-LABEL:    define{{.*}} void @_ZN5test04testEv()
//   The constructor is emitted as a call unwindabort.
// CHECK:      call unwindabort void @_ZN5test01AC1Ev(
//   So is the body call.
// CHECK:      call unwindabort void @_ZN5test03fooEv()
//   Destructors don't throw by default in C++11, so this is a plain call.
// CHECK:      call void @_ZN5test01AD1Ev(
// CHECK-NEXT: ret void

//   No terminate landingpad is generated, so __clang_call_terminate is not emitted.
// CHECK-NOT: @__clang_call_terminate
