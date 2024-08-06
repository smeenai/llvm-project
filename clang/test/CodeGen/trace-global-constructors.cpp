// RUN: %clang_cc1 -triple aarch64-none-linux-android23 -ftrace-global-constructors -emit-llvm -o - %s | \
// RUN:   FileCheck --check-prefix=TRACE -DFILENAME=%s %s
// RUN: %clang_cc1 -triple aarch64-none-linux-android23 -emit-llvm -o - %s | \
// RUN:   FileCheck --check-prefix=NO-TRACE -DFILENAME=%s %s

// NO-TRACE-NOT: @trace.init
// NO-TRACE-NOT: call void @ATrace_beginSection
// NO-TRACE-NOT: call void @ATrace_endSection

void external(void *);

void not_constructor() { external(nullptr); }

// TRACE: @trace.init = private unnamed_addr constant [[[#]] x i8] c"staticattr: [[FILENAME]]:[[@LINE+1]]:1\00"
__attribute__((constructor)) void constructor() { not_constructor(); }

struct HasConstructor { HasConstructor(); };

// TRACE: @trace.init.1 = private unnamed_addr constant [[[#]] x i8] c"staticinit: [[FILENAME]]:[[@LINE+1]]:1\00"
HasConstructor globalConstructor1;

// TRACE: @trace.init.3 = private unnamed_addr constant [[[#]] x i8] c"staticinit: [[FILENAME]]:[[@LINE+1]]:1\00"
HasConstructor globalConstructor2;

struct AlsoHasConstructor { AlsoHasConstructor(); };

// TRACE: @trace.init.5 = private unnamed_addr constant [[[#]] x i8] c"staticinit: [[FILENAME]]:[[@LINE+1]]:1\00"
AlsoHasConstructor globalConstructor3;

[[clang::lazy_init]] HasConstructor lazyInitGlobalConstructor;

// TRACE-LABEL: define dso_local void @_Z15not_constructorv() #[[#]] {
// TRACE-NEXT:  entry:
// TRACE-NEXT:    call void @_Z8externalPv(ptr noundef null)
// TRACE-NEXT:    ret void
// TRACE-NEXT:  }

// TRACE: declare void @_Z8externalPv(ptr noundef) #[[#DECL_ATTRS:]]

// TRACE-LABEL: define dso_local void @_Z11constructorv() #[[#]] {
// TRACE-NEXT:  entry:
// TRACE-NEXT:    call void @ATrace_beginSection(ptr noundef @trace.init)
// TRACE-NEXT:    call void @_Z15not_constructorv()
// TRACE-NEXT:    call void @ATrace_endSection()
// TRACE-NEXT:    ret void
// TRACE-NEXT:  }

// TRACE: declare void @ATrace_beginSection(ptr noundef) #[[#DECL_ATTRS]]

// TRACE: declare void @ATrace_endSection() #[[#DECL_ATTRS]]

// TRACE-LABEL: define internal void @__cxx_global_var_init() #[[#]] section ".text.startup" {
// TRACE-NEXT:  entry:
// TRACE-NEXT:    call void @ATrace_beginSection(ptr noundef @trace.init.1)
// TRACE-NEXT:    call void @_ZN14HasConstructorC1Ev(ptr noundef {{.*}} @globalConstructor1)
// TRACE-NEXT:    call void @ATrace_endSection()
// TRACE-NEXT:    ret void
// TRACE-NEXT:  }

// TRACE-LABEL: define internal void @__cxx_global_var_init.2() #[[#]] section ".text.startup" {
// TRACE-NEXT:  entry:
// TRACE-NEXT:    call void @ATrace_beginSection(ptr noundef @trace.init.3)
// TRACE-NEXT:    call void @_ZN14HasConstructorC1Ev(ptr noundef {{.*}} @globalConstructor2)
// TRACE-NEXT:    call void @ATrace_endSection()
// TRACE-NEXT:    ret void
// TRACE-NEXT:  }

// TRACE-LABEL: define internal void @__cxx_global_var_init.4() #[[#]] section ".text.startup" {
// TRACE-NEXT:  entry:
// TRACE-NEXT:    call void @ATrace_beginSection(ptr noundef @trace.init.5)
// TRACE-NEXT:    call void @_ZN18AlsoHasConstructorC1Ev(ptr noundef {{.*}} @globalConstructor3)
// TRACE-NEXT:    call void @ATrace_endSection()
// TRACE-NEXT:    ret void
// TRACE-NEXT:  }

// TRACE-LABEL: define {{.*}} @lazyInitGlobalConstructor.lazyinit()
// TRACE-NOT:     call void @ATrace_beginSection
// TRACE-NOT:     call void @ATrace_endSection

// TRACE-LABEL: define {{.*}} @_Z28hasFunctionStaticConstructorv()
// TRACE-NOT:     call void @ATrace_beginSection
// TRACE-NOT:     call void @ATrace_endSection

HasConstructor &hasFunctionStaticConstructor() {
  static HasConstructor not_global;
  return not_global;
}

// TRACE-LABEL: define {{.*}} @_Z19hasLocalConstructorv()
// TRACE-NOT:     call void @ATrace_beginSection
// TRACE-NOT:     call void @ATrace_endSection

void hasLocalConstructor() {
  HasConstructor also_not_global;
  external(&also_not_global);
}

// TRACE-LABEL: define internal void @_GLOBAL__sub_I_trace_global_constructors.cpp() #[[#]] section ".text.startup" {
// TRACE-NEXT:  entry:
// TRACE-NEXT:    call void @__cxx_global_var_init()
// TRACE-NEXT:    call void @__cxx_global_var_init.2()
// TRACE-NEXT:    call void @__cxx_global_var_init.4()
// TRACE-NEXT:    ret void
// TRACE-NEXT:  }

// TRACE: !llvm.dependent-libraries = !{![[#LIB_METADATA:]]}
// TRACE: ![[#LIB_METADATA]] = !{!"android"}
