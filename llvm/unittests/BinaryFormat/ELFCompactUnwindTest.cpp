//===- ELFCompactUnwindTest.cpp -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/BinaryFormat/ELFCompactUnwind.h"
#include "llvm/ADT/StringExtras.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::ELFCompactUnwind::AArch64;

namespace {

bool sameRules(const Frame &A, const Frame &B) {
  return A.CFAIsFP == B.CFAIsFP && A.CFAOffset == B.CFAOffset &&
         A.Saved == B.Saved && A.RASigned == B.RASigned && A.X18 == B.X18;
}

// Encoders check a region at frameChanges' points only, so frameAt must not
// change anywhere else.
TEST(ELFCompactUnwindTest, FrameChanges) {
  const uint64_t X19 = 1 << 8, X20 = 1 << 9, X29 = 1 << 18, X30 = 1 << 19,
                 Record = 1 << 20, D8 = 1 << 21;
  unsigned Shapes = 0;
  for (uint64_t Mode : {ModeFP, ModeSP})
    for (uint64_t Conv : {0, 1, 2, 3})
      for (uint64_t RA : {0, 1, 3, 5})
        for (uint64_t Regs :
             {X29 | X30 | Record, X19 | X20 | X29 | X30 | Record, X30,
              X19 | X30 | D8})
          for (uint64_t N : {0, 2, 5})
            for (uint64_t Windows : {0, 1, 2, 3})
              for (uint64_t PS : {0, 3, 7})
                for (uint64_t Apos : {0, 1, 2, 3})
                  for (uint64_t TailLen : {0, 1, 4})
                    for (uint64_t TailSigned : {0, 1}) {
                      uint64_t Desc = Mode | Conv << ConvShift | RA << RAShift |
                                      Regs | N << NShift |
                                      Windows << HasPrologueShift |
                                      PS << PSShift | Apos << AposShift |
                                      TailLen << TailLenShift |
                                      TailSigned << TailSignedShift;
                      Shape S;
                      if (!decodeShape(Desc, 0, S))
                        continue;
                      ++Shapes;
                      for (unsigned Len : {1, 4, 9, 16, 30}) {
                        SmallVector<unsigned, 8> Changes = frameChanges(S, Len);
                        ASSERT_FALSE(Changes.empty());
                        EXPECT_EQ(Changes[0], 0u);
                        size_t K = 0;
                        for (unsigned I = 0; I < Len; ++I) {
                          if (K + 1 < Changes.size() && Changes[K + 1] == I)
                            ++K;
                          EXPECT_TRUE(sameRules(frameAt(S, Len, I),
                                                frameAt(S, Len, Changes[K])))
                              << "descriptor 0x" << utohexstr(Desc) << ", "
                              << Len << " instructions, at " << I;
                        }
                      }
                    }
  // Most combinations are valid shapes.
  EXPECT_GT(Shapes, 10000u);
}

} // namespace
