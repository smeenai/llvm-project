// -*- C++ -*-
//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Unwinding with AArch64 ELF compact unwind tables, which lld writes in place
// of .eh_frame: each frame's return address and a callee-saved register are
// recovered through frames of different shapes.

// REQUIRES: lld, target={{aarch64-.+-linux.*}}
// RUN: %{build} --ld-path=%{lld} -Wl,-z,compact-unwind=async
// RUN: %{exec} %t.exe
// RUN: %{build} --ld-path=%{lld} -Wl,-z,compact-unwind=sync
// RUN: %{exec} %t.exe

#include <alloca.h>
#include <libunwind.h>
#include <stdint.h>
#include <stdlib.h>

// Each frame's return address, outermost first.
static void *returnAddresses[8];
static int depth;

#define RECORD_RETURN_ADDRESS()                                                \
  returnAddresses[depth++] =                                                   \
      __builtin_extract_return_addr(__builtin_return_address(0))
// Make the function save x19 and d8.
#define CLOBBER_CALLEE_SAVED() asm volatile("" ::: "x19", "d8")

__attribute__((noinline)) static void walk() {
  RECORD_RETURN_ADDRESS();
  CLOBBER_CALLEE_SAVED();
  unw_context_t context;
  unw_cursor_t cursor;
  unw_getcontext(&context);
  unw_init_local(&cursor, &context);
  for (int i = depth - 1; i >= 0; --i) {
    if (unw_step(&cursor) <= 0)
      abort();
    unw_word_t ip;
    unw_get_reg(&cursor, UNW_REG_IP, &ip);
    if ((void *)ip != returnAddresses[i])
      abort();
    // In outer's frame, x19 holds the value outer put there.
    unw_word_t x19;
    unw_get_reg(&cursor, UNW_AARCH64_X19, &x19);
    if (i == 1 && x19 != 0x1919)
      abort();
  }
}

// A frame whose return address is signed.
__attribute__((noinline, target("branch-protection=pac-ret"))) static void
signedReturnAddress() {
  RECORD_RETURN_ADDRESS();
  CLOBBER_CALLEE_SAVED();
  walk();
}

// A frame with a variable-sized allocation, whose CFA is x29-based.
__attribute__((noinline)) static void dynamicAlloca(int n) {
  RECORD_RETURN_ADDRESS();
  CLOBBER_CALLEE_SAVED();
  volatile char *p = static_cast<char *>(alloca(n));
  p[0] = 0;
  signedReturnAddress();
}

__attribute__((noinline)) static void outer() {
  RECORD_RETURN_ADDRESS();
  register uintptr_t x19 asm("x19") = 0x1919;
  asm volatile("" : "+r"(x19));
  dynamicAlloca(16);
  asm volatile("" ::"r"(x19));
}

int main(int, char **) {
  outer();
  return 0;
}
