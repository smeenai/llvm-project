// RUN: %clang_cc1 -triple aarch64-none-linux-android21 -emit-llvm -o - %s | FileCheck %s

struct NonTrivial {
  NonTrivial();
  NonTrivial(int);
  ~NonTrivial();
  NonTrivial &operator=(const NonTrivial &);
  void doSomething();

  // So that the compiler-generated copy constructor has something to do.
  int i;
};

// CHECK: @lazy_global = global %struct.NonTrivial zeroinitializer, align 4
// CHECK-NEXT: @_ZGV11lazy_global = global i64 0, align 8
[[clang::lazy_init]] NonTrivial lazy_global;

// CHECK: @lazy_global_copy = global %struct.NonTrivial zeroinitializer, align 4
// CHECK: @lazy_global_pointer = global ptr null, align 8
// CHECK: @lazy_global_reference = global ptr null, align 8

// See the definition of the above variables below for why we still have a global ctor.
// CHECK: @llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [{ i32, ptr, ptr } { i32 65535, ptr @_GLOBAL__sub_I_lazy_init.cpp, ptr null }]

// CHECK-LABEL: define noundef ptr @lazy_global.lazyinit() #[[#]] {
// CHECK-NEXT:  entry:
// CHECK-NEXT:    %0 = load atomic i8, ptr @_ZGV11lazy_global acquire, align 8
// CHECK-NEXT:    %1 = and i8 %0, 1
// CHECK-NEXT:    %guard.uninitialized = icmp eq i8 %1, 0
// CHECK-NEXT:    br i1 %guard.uninitialized, label %init.check, label %init.end
// CHECK-EMPTY:
// CHECK-NEXT:  init.check:
// CHECK-NEXT:    %2 = call i32 @__cxa_guard_acquire(ptr @_ZGV11lazy_global)
// CHECK-NEXT:    %tobool = icmp ne i32 %2, 0
// CHECK-NEXT:    br i1 %tobool, label %init, label %init.end
// CHECK-EMPTY:
// CHECK-NEXT:  init:
// CHECK-NEXT:    call void @_ZN10NonTrivialC1Ev(ptr noundef nonnull align 4 dereferenceable(4) @lazy_global)
// CHECK-NEXT:    %3 = call i32 @__cxa_atexit(ptr @_ZN10NonTrivialD1Ev, ptr @lazy_global, ptr @__dso_handle)
// CHECK-NEXT:    call void @__cxa_guard_release(ptr @_ZGV11lazy_global)
// CHECK-NEXT:    br label %init.end
// CHECK-EMPTY:
// CHECK-NEXT:  init.end:
// CHECK-NEXT:    ret ptr @lazy_global
// CHECK-NEXT:  }

// CHECK-LABEL: define dso_local void @_Z11doSomethingv() #[[#]] {
// CHECK-NEXT:  entry:
// CHECK-NEXT:    %0 = call ptr @lazy_global.lazyinit()
// CHECK-NEXT:    call void @_ZN10NonTrivial11doSomethingEv(ptr noundef nonnull align 4 dereferenceable(4) %0)
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }
void doSomething() {
  lazy_global.doSomething();
}

// Check that constructor arguments are passed correctly.
// CHECK-LABEL: define noundef ptr @lazy_global_with_args.lazyinit() #[[#]] {
// CHECK: call void @_ZN10NonTrivialC1Ei(ptr noundef nonnull align 4 dereferenceable(4) @lazy_global_with_args, i32 noundef 4)
[[clang::lazy_init]] NonTrivial lazy_global_with_args(4);

// Check that the init func has the same linkage and visibility as the lazy_init variable.
// CHECK-LABEL: define internal noundef ptr @_ZL11lazy_static.lazyinit()
[[clang::lazy_init]] static NonTrivial lazy_static;

// CHECK-LABEL: define hidden noundef ptr @lazy_hidden_global.lazyinit
[[clang::lazy_init, gnu::visibility("hidden")]] NonTrivial lazy_hidden_global;

// Check that the attribute is respected on declarations.
// CHECK-NOT: define hidden noundef ptr @lazy_extern.lazyinit()
[[clang::lazy_init]] extern NonTrivial lazy_extern;

// CHECK-LABEL: define dso_local void @_Z17doSomethingExternv() #[[#]] {
// CHECK-NEXT:  entry:
// CHECK-NEXT:    %0 = call ptr @lazy_extern.lazyinit()
// CHECK-NEXT:    call void @_ZN10NonTrivial11doSomethingEv(ptr noundef nonnull align 4 dereferenceable(4) %0)
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }
void doSomethingExtern() {
  lazy_extern.doSomething();
}

// Check that writes go through the init func (so that e.g. an assignment
// operator doesn't access an uninitialized object).

// CHECK-LABEL: define dso_local void @_Z12doAssignmentv() #[[#]] {
// CHECK-NEXT:  entry:
// CHECK-NEXT:    %ref.tmp = alloca %struct.NonTrivial, align 4
// CHECK-NEXT:    call void @_ZN10NonTrivialC1Ev(ptr noundef nonnull align 4 dereferenceable(4) %ref.tmp)
// CHECK-NEXT:    %0 = call ptr @lazy_global.lazyinit()
// CHECK-NEXT:    %call = call {{.*}} ptr @_ZN10NonTrivialaSERKS_(ptr {{.*}} %0, ptr {{.*}} %ref.tmp)
// CHECK-NEXT:    call void @_ZN10NonTrivialD1Ev(ptr noundef nonnull align 4 dereferenceable(4) %ref.tmp)
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }
void doAssignment() {
  lazy_global = NonTrivial();
}

// Check that variables templates work as expected.

// CHECK-LABEL: define dso_local void @_Z21referenceLazyTemplatev() #[[#]] {
// CHECK-NEXT:  entry:
// CHECK-NEXT:    %0 = call ptr @_Z13lazy_templateI10NonTrivialE.lazyinit()
// CHECK-NEXT:    call void @_ZN10NonTrivial11doSomethingEv(ptr noundef nonnull align 4 dereferenceable(4) %0)
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }

// CHECK-LABEL: define linkonce_odr noundef ptr @_Z13lazy_templateI10NonTrivialE.lazyinit() #[[#]] comdat {
// CHECK:         call void @_ZN10NonTrivialC1Ev(ptr noundef nonnull align 4 dereferenceable(4) @_Z13lazy_templateI10NonTrivialE)
template <class T>
[[clang::lazy_init]] T lazy_template;

void referenceLazyTemplate() {
  lazy_template<NonTrivial>.doSomething();
}

// Check that the implicit copy constructor calls the initializer before copying.
// CHECK-LABEL: define dso_local void @_Z6doCopyv() #[[#]] {
// CHECK-NEXT:  entry:
// CHECK-NEXT:    %copy = alloca %struct.NonTrivial, align 4
// CHECK-NEXT:    %0 = call ptr @lazy_global.lazyinit()
// CHECK-NEXT:    call void @llvm.memcpy.p0.p0.i64(ptr align 4 %copy, ptr align 4 %0, i64 4, i1 false)
// CHECK-NEXT:    call void @_ZN10NonTrivialD1Ev(ptr noundef nonnull align 4 dereferenceable(4) %copy)
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }
void doCopy() {
  NonTrivial copy(lazy_global);
}

// Check that forming a pointer calls the initializer.
// CHECK-LABEL: define dso_local noundef ptr @_Z10getPointerv() #[[#]] {
// CHECK-NEXT:  entry:
// CHECK-NEXT:    %0 = call ptr @lazy_global.lazyinit()
// CHECK-NEXT:    ret ptr %0
// CHECK-NEXT:  }
NonTrivial *getPointer() { return &lazy_global; }

// Check that forming a reference calls the initializer.
// CHECK-LABEL: define dso_local noundef nonnull align 4 dereferenceable(4) ptr @_Z12getReferencev() #[[#]] {
// CHECK-NEXT:  entry:
// CHECK-NEXT:    %0 = call ptr @lazy_global.lazyinit()
// CHECK-NEXT:    ret ptr %0
// CHECK-NEXT:  }
NonTrivial &getReference() { return lazy_global; }

// Check that other global variables initialized with a lazy global (by copy,
// pointer, or reference) go through the initializer instead of referencing the
// global directly. This necessitates a global constructor, but there's no way
// around that, and this should be pretty rare in our actual apps.

// CHECK-LABEL: define internal void @__cxx_global_var_init() #[[#]] section ".text.startup" {
// CHECK-NEXT:  entry:
// CHECK-NEXT:    %0 = call ptr @lazy_global.lazyinit()
// CHECK-NEXT:    call void @llvm.memcpy.p0.p0.i64(ptr align 4 @lazy_global_copy, ptr align 4 %0, i64 4, i1 false)
// CHECK-NEXT:    %1 = call i32 @__cxa_atexit(ptr @_ZN10NonTrivialD1Ev, ptr @lazy_global_copy, ptr @__dso_handle)
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }

// CHECK-LABEL: define internal void @__cxx_global_var_init.1() #[[#]] section ".text.startup" {
// CHECK-NEXT:  entry:
// CHECK-NEXT:    %0 = call ptr @lazy_global.lazyinit()
// CHECK-NEXT:    store ptr %0, ptr @lazy_global_pointer, align 8
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }

// CHECK-LABEL: define internal void @__cxx_global_var_init.2() #[[#]] section ".text.startup" {
// CHECK-NEXT:  entry:
// CHECK-NEXT:    %0 = call ptr @lazy_global.lazyinit()
// CHECK-NEXT:    store ptr %0, ptr @lazy_global_reference, align 8
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }

NonTrivial lazy_global_copy(lazy_global);
NonTrivial *lazy_global_pointer = &lazy_global;
NonTrivial &lazy_global_reference = lazy_global;

// Check that a lazy global referencing another lazy global doesn't create a
// global constructor (GLOBAL__sub_I below won't reference the lazy initializer).
// CHECK-LABEL: define noundef ptr @lazy_global_lazy_copy.lazyinit()
[[clang::lazy_init]] NonTrivial lazy_global_lazy_copy(lazy_global);

// CHECK-LABEL: define internal void @_GLOBAL__sub_I_lazy_init.cpp() #[[#]] section ".text.startup" {
// CHECK-NEXT:  entry:
// CHECK-NEXT:    call void @__cxx_global_var_init()
// CHECK-NEXT:    call void @__cxx_global_var_init.1()
// CHECK-NEXT:    call void @__cxx_global_var_init.2()
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }
