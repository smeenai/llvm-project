// RUN: %clang_cc1 -verify -fsyntax-only %s

struct S { S(); };

[[clang::lazy_init]] S i;
[[clang::lazy_init]] static S j;
[[clang::lazy_init]] extern S k;
template <class T>
[[clang::lazy_init]] T l;
struct T {
  [[clang::lazy_init]] static S m;
};

[[clang::lazy_init]] thread_local S n; // expected-error {{'clang::lazy_init' attribute only applies to non-TLS defined scalar global variables}}
[[clang::lazy_init]] S o[1]; // expected-error {{'clang::lazy_init' attribute only applies to non-TLS defined scalar global variables}}
void f([[clang::lazy_init]] S p) { // expected-error {{'clang::lazy_init' attribute only applies to non-TLS defined scalar global variables}}
  [[clang::lazy_init]] S q; // expected-error {{'clang::lazy_init' attribute only applies to non-TLS defined scalar global variables}}
  [[clang::lazy_init]] static S r; // expected-error {{'clang::lazy_init' attribute only applies to non-TLS defined scalar global variables}}
}

[[clang::lazy_init]] int s; // expected-error {{'lazy_init' attribute only applies to dynamic initializers}}
[[clang::lazy_init]] int t = 1; // expected-error {{'lazy_init' attribute only applies to dynamic initializers}}

struct U { int i; };
[[clang::lazy_init]] U u; // expected-error {{'lazy_init' attribute only applies to dynamic initializers}}
[[clang::lazy_init]] U v{1}; // expected-error {{'lazy_init' attribute only applies to dynamic initializers}}

struct V { V() = default; };
[[clang::lazy_init]] V w; // expected-error {{'lazy_init' attribute only applies to dynamic initializers}}

struct W {
  ~W();
};
// This requires a global constructor to register the destructor so lazy_init applies.
[[clang::lazy_init]] W x;
