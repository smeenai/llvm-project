// RUN: %clang_cc1 -fms-extensions -fms-compatibility-version=19.20 -triple x86_64-windows-msvc -fexceptions -fcxx-exceptions -emit-llvm -o - %s | FileCheck %s --check-prefix=MSVC
// RUN: %clang_cc1 -fms-extensions -fms-compatibility-version=19.20 -triple aarch64-windows-msvc -fexceptions -fcxx-exceptions -emit-llvm -o - %s | FileCheck %s --check-prefix=MSVC
// RUN: %clang_cc1 -triple x86_64-windows-itanium -fexceptions -fcxx-exceptions -emit-llvm -o - %s | FileCheck %s --check-prefix=ITANIUM
// RUN: %clang_cc1 -triple aarch64-windows-gnu -fexceptions -fcxx-exceptions -emit-llvm -o - %s | FileCheck %s --check-prefix=GNU

void foo1() { throw 1; }
// _CxxThrowException should not be marked dllimport.
// MSVC-LABEL: define dso_local void @"?foo1@@YAXXZ"
// MSVC: call void @_CxxThrowException
// MSVC: declare dso_local void @_CxxThrowException(ptr, ptr)

// __cxa_throw should be marked dllimport for *-windows-itanium.
// ITANIUM-LABEL: define dso_local void @_Z4foo1v()
// ITANIUM: call void @__cxa_throw({{.*}})
// ITANIUM: declare dllimport void @__cxa_throw({{.*}})

// ... but not for *-windows-gnu.
// GNU-LABEL: define dso_local void @_Z4foo1v()
// GNU: call void @__cxa_throw({{.*}})
// GNU: declare dso_local void @__cxa_throw({{.*}})


void bar();
void foo2() noexcept(true) { bar(); }
// On MSVC, a potentially-throwing call in a noexcept function still terminates
// via __std_terminate (which should not be marked dllimport).
// MSVC-LABEL: define dso_local void @"?foo2@@YAXXZ"
// MSVC: call void @__std_terminate()
// MSVC: declare dso_local void @__std_terminate()

// On Itanium/GNU the call is emitted as a 'call unwindabort', so neither
// __clang_call_terminate nor _ZSt9terminatev / __cxa_begin_catch is emitted.
// ITANIUM-LABEL: define dso_local void @_Z4foo2v()
// ITANIUM: call unwindabort void @_Z3barv()
// ITANIUM-NOT: @__clang_call_terminate
// ITANIUM-NOT: @_ZSt9terminatev

// GNU-LABEL: define dso_local void @_Z4foo2v()
// GNU: call unwindabort void @_Z3barv()
// GNU-NOT: @__clang_call_terminate
// GNU-NOT: @_ZSt9terminatev


struct A {};
struct B { virtual void f(); };
struct C : A, virtual B {};
struct T {};
T *foo3() { return dynamic_cast<T *>((C *)0); }
// __RTDynamicCast should not be marked dllimport.
// MSVC-LABEL: define dso_local noundef ptr @"?foo3@@YAPEAUT@@XZ"
// MSVC: call ptr @__RTDynamicCast({{.*}})
// MSVC: declare dso_local ptr @__RTDynamicCast(ptr, i32, ptr, ptr, i32)

// Again, imported
// ITANIUM-LABEL: define dso_local noundef ptr @_Z4foo3v()
// ITANIUM: call ptr @__dynamic_cast({{.*}})
// ITANIUM: declare dllimport ptr @__dynamic_cast({{.*}})

// Not imported
// GNU-LABEL: define dso_local noundef ptr @_Z4foo3v()
// GNU: call ptr @__dynamic_cast({{.*}})
// GNU: declare dso_local ptr @__dynamic_cast({{.*}})
