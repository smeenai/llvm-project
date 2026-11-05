//===- CompactUnwind.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_COMPACT_UNWIND_H
#define LLD_ELF_COMPACT_UNWIND_H

#include "lld/Common/LLVM.h"
#include <memory>

namespace lld::elf {
struct Ctx;

// The table refers to personality routines through pointer-sized slots, so a
// routine that a CIE references directly needs a GOT entry. Called after
// relocation scanning, before GOT entries are allocated.
void requestCompactUnwindGotEntries(Ctx &ctx);

// AArch64 compact unwind table, written into .eh_frame_hdr in place of the
// binary search table over .eh_frame, which isn't output. Encoding the CFI
// doesn't depend on addresses and is done once; the paged layout depends on
// function addresses and is recomputed whenever they change.
class CompactUnwindTable {
public:
  explicit CompactUnwindTable(Ctx &ctx);
  ~CompactUnwindTable();
  void encode();
  size_t layout(uint64_t hdrVA);
  // Writes the table using the most recent layout.
  void write(uint8_t *buf, uint64_t hdrVA);

  struct Impl;

private:
  std::unique_ptr<Impl> impl;
};
} // namespace lld::elf

#endif
