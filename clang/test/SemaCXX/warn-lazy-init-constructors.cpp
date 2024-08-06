// RUN: %clang_cc1 -verify -fsyntax-only -Wglobal-constructors %s
struct S { S(); };
[[clang::lazy_init]] S s; // expected-no-diagnostics
