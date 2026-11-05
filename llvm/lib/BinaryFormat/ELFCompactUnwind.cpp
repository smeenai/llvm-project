//===- llvm/BinaryFormat/ELFCompactUnwind.cpp -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/BinaryFormat/ELFCompactUnwind.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/bit.h"
#include <algorithm>

using namespace llvm;
using namespace llvm::ELFCompactUnwind;
using namespace llvm::ELFCompactUnwind::AArch64;

// LLVM: consecutive 8-byte slots downward from CFA - Top: x19..x28 (plus x30
// and x29 when there is no frame record), then the x30/x29 frame record, then
// d8..d15. If the total is odd, the last register of the odd-sized group sits
// below an 8-byte hole.
static void llvmSlots(uint32_t Regs, int Top, bool FrameRecord, SaveSlots &S) {
  int Order[NumCSRs], N = 0;
  for (int I = 0; I < CSRX29; ++I)
    if (Regs >> I & 1)
      Order[N++] = I;
  if (!FrameRecord) {
    if (Regs >> CSRX30 & 1)
      Order[N++] = CSRX30;
    if (Regs >> CSRX29 & 1)
      Order[N++] = CSRX29;
  }
  int NumGPRs = N;
  if (FrameRecord) {
    Order[N++] = CSRX30;
    Order[N++] = CSRX29;
  }
  int DStart = N;
  for (int I = 12; I < NumCSRs; ++I)
    if (Regs >> I & 1)
      Order[N++] = I;
  int NumDRegs = N - DStart;
  int Hole = -1;
  if (N % 2)
    Hole = NumGPRs % 2 ? Order[NumGPRs - 1] : NumDRegs % 2 ? Order[N - 1] : -1;
  int Off = -Top;
  for (int K = 0; K < N; ++K) {
    if (Order[K] == Hole)
      Off -= 8;
    Off -= 8;
    S.set(Order[K], Off);
  }
}

// GCC: upward from Base, the GPRs with a frame record first, otherwise x30
// first and x29 last; then d8..d15 from the next 16-byte boundary, except that
// the last FP register fills the alignment gap when it would start 16-byte
// aligned (aarch64_layout_frame).
static void gccSlots(uint32_t Regs, int Base, bool FrameRecord, SaveSlots &S) {
  int Order[12], N = 0;
  if (FrameRecord) {
    Order[N++] = CSRX29;
    Order[N++] = CSRX30;
  } else if (Regs >> CSRX30 & 1) {
    Order[N++] = CSRX30;
  }
  for (int I = 0; I < CSRX29; ++I)
    Order[N++] = I;
  if (!FrameRecord)
    Order[N++] = CSRX29;
  int Off = Base;
  for (int K = 0; K < N; ++K)
    if (Regs >> Order[K] & 1) {
      S.set(Order[K], Off);
      Off += 8;
    }
  int GPREnd = Off;
  Off = Base + ((Off - Base + 15) & ~15);
  bool Gap = Off != GPREnd;
  int DRegs[8], ND = 0;
  for (int I = 12; I < NumCSRs; ++I)
    if (Regs >> I & 1)
      DRegs[ND++] = I;
  for (int K = 0; K < ND; ++K) {
    if (K == ND - 1 && Gap && (Off - Base) % 16 == 0) {
      S.set(DRegs[K], GPREnd);
      break;
    }
    S.set(DRegs[K], Off);
    Off += 8;
  }
}

// GCC's order with the save area ending Top bytes below the CFA (stack
// protector frames, aarch64_save_regs_above_locals_p).
static void gccTopSlots(uint32_t Regs, int Top, bool FrameRecord,
                        SaveSlots &S) {
  SaveSlots T;
  gccSlots(Regs, 0, FrameRecord, T);
  if (!T.Mask)
    return;
  int Max = INT32_MIN;
  for (int R = 0; R < NumCSRs; ++R)
    if ((T.Mask >> R & 1) && T.Offsets[R] > Max)
      Max = T.Offsets[R];
  int Size = (Max + 8 + 15) & ~15;
  for (int R = 0; R < NumCSRs; ++R)
    if (T.Mask >> R & 1)
      S.set(R, T.Offsets[R] - Size - Top);
}

static uint32_t descRegs(uint64_t Desc) {
  return uint32_t((Desc >> RegsShift) & 0xfff) |
         uint32_t((Desc >> DRegsShift) & 0xff) << 12;
}

bool AArch64::decodeShape(uint64_t Desc, uint64_t Layout, Shape &S) {
  S = Shape();
  S.Mode = Desc & 7;
  if (S.Mode == ModeEscape || S.Mode > ModePartialSP)
    return false;
  S.Partial = S.Mode == ModePartialFP || S.Mode == ModePartialSP;
  S.FP = S.Mode == ModeFP || S.Mode == ModePartialFP;
  unsigned RAField = (Desc >> RAShift) & 7;
  S.RA = RAField != 0;
  S.WithPC = RAField >= 3;
  // PARTIAL descriptors take the convention from their layout descriptor and
  // use their own field only for the shadow call stack (0 or 1).
  unsigned OwnConv = (Desc >> ConvShift) & 3;
  S.SCS = OwnConv == 1;
  S.N = (Desc >> NShift) & 0xfff;
  uint64_t Lay = S.Partial ? Layout : Desc;
  unsigned LayConv = (Lay >> ConvShift) & 3;
  S.Conv = LayConv < 2 ? ConvLLVM : LayConv == 2 ? ConvGCC : ConvGCCTop;
  S.O = (Lay >> OShift) & 0xf;
  S.LayoutRegs = descRegs(Lay);
  uint32_t OwnRegs = descRegs(Desc);
  S.FrameRecord = (Lay >> FrameRecordShift) & 1;
  if ((S.FrameRecord && (S.LayoutRegs & FrameRecordCSRs) != FrameRecordCSRs) ||
      (S.Partial &&
       (((Desc >> FrameRecordShift) & 1) || OwnConv > 1 ||
        (S.SCS && S.Conv != ConvLLVM) || (OwnRegs & ~S.LayoutRegs))))
    return false;
  if (S.Mode != ModeNull) {
    if (S.Conv == ConvLLVM)
      llvmSlots(S.LayoutRegs, 16 * int(S.O), S.FrameRecord, S.Slots);
    else if (S.Conv == ConvGCCTop)
      gccTopSlots(S.LayoutRegs, 16 * int(S.O), S.FrameRecord, S.Slots);
    else
      gccSlots(S.LayoutRegs, -16 * int(S.N) + 16 * int(S.O), S.FrameRecord,
               S.Slots);
    if (S.FP && !(S.Slots.Mask >> CSRX29 & 1))
      return false;
  }
  S.Saved = S.Mode == ModeNull ? 0 : S.Partial ? OwnRegs : S.LayoutRegs;
  S.Area = 0;
  for (int R = 0; R < NumCSRs; ++R)
    if ((S.Slots.Mask >> R & 1) && -S.Slots.Offsets[R] > S.Area)
      S.Area = -S.Slots.Offsets[R];
  computeLengths(Desc, S);
  return true;
}

void AArch64::computeLengths(uint64_t Desc, Shape &S) {
  // The lead-in is the signing instructions (PAuth_LR signs with pacm;
  // paciasp); the shadow call stack push follows it. The tail is the shadow
  // call stack pop, then before the return PAuth_LR materializes the signing
  // address (adrp, add) and issues pacm, and autiasp follows unless the return
  // authenticates.
  bool Windows = !S.Partial && S.Mode != ModeNull;
  S.HasPrologue = Windows && ((Desc >> HasPrologueShift) & 1);
  S.HasEpilogue = Windows && ((Desc >> HasEpilogueShift) & 1);
  S.TailSigned = S.HasEpilogue && ((Desc >> TailSignedShift) & 1);
  unsigned AposOverride = S.HasPrologue ? (Desc >> AposShift) & 3 : 0;
  unsigned PSOverride = S.HasPrologue ? (Desc >> PSShift) & 0xf : 0;
  unsigned TailLenOverride = S.HasEpilogue ? (Desc >> TailLenShift) & 0xf : 0;
  unsigned SCS = S.SCS ? 1 : 0;
  S.CanonApos = S.RA ? (S.WithPC ? 2 : 1) : 0;
  S.Apos = AposOverride ? AposOverride - 1 : S.CanonApos;
  unsigned Auth = S.RA ? (S.WithPC ? 3 : 0) + (S.TailSigned ? 0 : 1) : 0;
  S.CanonTailLen = 1 + Auth + SCS;
  S.TailLen = TailLenOverride ? TailLenOverride : S.CanonTailLen;
  if (S.Conv == ConvLLVM) {
    uint32_t Pool = S.LayoutRegs & 0x3ff;
    if (!S.FrameRecord)
      Pool |= S.LayoutRegs & FrameRecordCSRs;
    unsigned Groups = (popcount(Pool) + 1) / 2 + (S.FrameRecord ? 1 : 0) +
                      (popcount(S.LayoutRegs >> 12) + 1) / 2;
    bool PreIndex = 16 * int(S.N) == S.Area && Groups > 0;
    S.AKind = false;
    S.CanonPS = S.Apos + SCS + (PreIndex ? 0 : 1) + Groups + (S.FP ? 1 : 0);
    S.EpiLen = S.FP ? (PreIndex ? Groups : Groups + 1) : 0;
  } else {
    // Only the allocating store is canonical; with a pre-index frame record
    // store, the record is in its slot from then on.
    bool PreIndex = S.Conv == ConvGCCTop
                        ? S.FrameRecord && S.O == 0 && !S.FP &&
                              16 * int(S.N) == S.Area && S.Area <= 512
                        : S.FrameRecord && S.O == 0 && 16 * int(S.N) <= 504;
    S.AKind = PreIndex;
    S.CanonPS = S.Apos + SCS + 1;
    S.EpiLen = 0;
  }
  S.PS = PSOverride ? PSOverride : S.CanonPS;
  if (!S.HasPrologue)
    S.PS = 0;
  if (!S.HasEpilogue)
    S.EpiLen = S.TailLen = 0;
}

Frame AArch64::frameAt(const Shape &S, unsigned N, unsigned I) {
  Frame F;
  F.RASigned = S.RA;
  F.X18 = S.SCS;
  F.Saved = S.Saved;
  if (S.Mode != ModeNull) {
    if (S.FP) {
      F.CFAIsFP = true;
      F.CFAOffset = -S.Slots.Offsets[CSRX29];
    } else {
      F.CFAOffset = 16 * int32_t(S.N);
    }
  }
  if (I < S.PS) {
    F.CFAIsFP = false;
    if (I <= S.Apos) {
      F.CFAOffset = 0;
      F.Saved = 0;
      F.RASigned = S.RA && I >= S.Apos;
      F.X18 = false;
    } else if (S.SCS && I == S.Apos + 1) {
      F.CFAOffset = 0;
      F.Saved = 0;
    } else {
      F.CFAOffset = 16 * int32_t(S.N);
      F.Saved = S.AKind ? (S.Slots.Mask & FrameRecordCSRs) : 0;
    }
    return F;
  }
  if (S.TailLen && I + S.TailLen >= N) {
    F.CFAIsFP = false;
    F.CFAOffset = 0;
    // Signed up to and including the authentication; instructions after it
    // (a tail call's argument moves) are unsigned.
    unsigned SignedLen = (S.SCS ? 1 : 0) + (S.WithPC ? 4 : 1);
    F.RASigned = S.RA && (S.TailSigned || I - (N - S.TailLen) < SignedLen);
    // At the shadow call stack pop the saved registers are still described in
    // their slots.
    F.X18 = S.SCS && I == N - S.TailLen;
    if (!F.X18)
      F.Saved = 0;
    return F;
  }
  if (S.EpiLen && I + S.TailLen + S.EpiLen >= N) {
    F.CFAIsFP = false;
    F.CFAOffset = 16 * int32_t(S.N);
  }
  return F;
}

SmallVector<unsigned, 8> AArch64::frameChanges(const Shape &S, unsigned N) {
  // Where each of frameAt's cases begins. A tail or epilogue window longer
  // than the region starts before it.
  SmallVector<unsigned, 8> Out = {0};
  auto Add = [&](int64_t I) {
    if (I > 0 && I < N)
      Out.push_back(I);
  };
  if (S.PS)
    for (int64_t I : {S.Apos, S.Apos + 1, S.Apos + 1 + S.SCS, S.PS})
      Add(I);
  int64_t Tail = int64_t(N) - S.TailLen;
  if (S.EpiLen)
    Add(Tail - S.EpiLen);
  if (S.TailLen)
    for (int64_t I :
         {Tail, Tail + 1, Tail + (S.SCS ? 1 : 0) + (S.WithPC ? 4 : 1)})
      Add(I);
  llvm::sort(Out);
  Out.erase(llvm::unique(Out), Out.end());
  return Out;
}

std::optional<unsigned> AArch64::pauthLRSite(uint64_t Desc, uint64_t Layout) {
  unsigned Mode = Desc & 7;
  bool Partial = Mode == ModePartialFP || Mode == ModePartialSP;
  bool HasPrologue = !Partial && ((Desc >> HasPrologueShift) & 1);
  bool HasEpilogue = !Partial && ((Desc >> HasEpilogueShift) & 1);
  // The signing instruction ends a prologue's lead-in, which is canonically
  // pacm; paciasp.
  unsigned AposOverride = (Desc >> AposShift) & 3;
  unsigned Apos = AposOverride ? AposOverride - 1 : 2;
  if (HasPrologue && Apos >= 1)
    return Apos - 1;
  // Else N holds it, region-relative, where nothing else uses N: GCC's
  // sp-anchored convention (2) uses N for its slots, and 4095 means it is out
  // of range.
  unsigned Conv = ((Partial ? Layout : Desc) >> ConvShift) & 3;
  unsigned N = (Desc >> NShift) & 0xfff;
  bool NFree = !HasPrologue && !HasEpilogue &&
               (Mode == ModeNull ||
                ((Mode == ModeFP || Mode == ModePartialFP) && Conv != 2));
  if (NFree && N != MaxN)
    return N;
  return std::nullopt;
}
