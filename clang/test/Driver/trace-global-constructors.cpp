// RUN: %clang -### -target aarch64-none-linux-android23 \
// RUN:   -fno-trace-global-constructors -ftrace-global-constructors %s 2>&1 | \
// RUN:   FileCheck --check-prefix=TRACE %s
// TRACE: "-ftrace-global-constructors"

// RUN: %clang -### -target aarch64-none-linux-android23 \
// RUN:   -ftrace-global-constructors -fno-trace-global-constructors %s 2>&1 | \
// RUN:   FileCheck --check-prefix=NO-TRACE %s
// NO-TRACE-NOT: "-ftrace-global-constructors"

// RUN: not %clang -### -target aarch64-none-linux-android22 -ftrace-global-constructors %s 2>&1 | \
// RUN:   FileCheck --check-prefix=INVALID %s
// RUN: not %clang -### -target x86_64-linux-gnu -ftrace-global-constructors %s 2>&1 | \
// RUN:   FileCheck --check-prefix=INVALID %s
// INVALID: error: unsupported option '-ftrace-global-constructors' for target
