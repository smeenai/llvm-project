//===- ELFCompactUnwindPrinter.h - ELF compact unwind table printer -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_READOBJ_ELFCOMPACTUNWINDPRINTER_H
#define LLVM_TOOLS_LLVM_READOBJ_ELFCOMPACTUNWINDPRINTER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>

namespace llvm {
class ScopedPrinter;

/// Prints the AArch64 compact unwind table that Table, the contents of the
/// PT_GNU_EH_FRAME segment at Address, holds: its header and side tables, and
/// for each region its descriptor and the unwind rows it implies.
void printELFCompactUnwindTable(ScopedPrinter &W, ArrayRef<uint8_t> Table,
                                uint64_t Address, bool IsLittleEndian,
                                StringRef FileName);

} // namespace llvm

#endif
