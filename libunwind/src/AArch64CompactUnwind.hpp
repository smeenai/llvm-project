//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Decoding of AArch64 ELF compact unwind descriptors: what a descriptor says
// about the frame at each instruction of its region. LLVM has its own copy of
// this logic (llvm/lib/BinaryFormat/ELFCompactUnwind.cpp), which LLD uses to
// check every descriptor it emits; the two must agree.
//
// Callee-saved registers are numbered x19-x28 = 0-9, x29 = 10, x30 = 11,
// d8-d15 = 12-19, which is also their order in the descriptor's masks.
//
//===----------------------------------------------------------------------===//

#ifndef __AARCH64COMPACTUNWIND_HPP__
#define __AARCH64COMPACTUNWIND_HPP__

#include <stdint.h>

namespace libunwind {
namespace aarch64_compact_unwind {

enum : unsigned {
  kModeNull = 0,
  kModeFp = 1,
  kModeSp = 2,
  kModeEscape = 3,
  kModePartialFp = 4,
  kModePartialSp = 5,
  kModeNoInfo = 6,
};

// Conventions as stored: 0 LLVM, 1 LLVM with a shadow call stack, 2 GCC
// sp-anchored, 3 GCC CFA-anchored.
enum Conv : unsigned { kLlvm, kGcc, kGccTop };

enum : int { kNumCsr = 20, kX29 = 10, kX30 = 11 };
static const uint32_t kRecordBits = (1u << kX29) | (1u << kX30);

// Field positions.
enum : unsigned {
  kConvShift = 3,
  kRaShift = 5,
  kRegsShift = 8,         // x19-x30
  kFrameRecordShift = 20, // x29 and x30 form a frame record
  kDRegsShift = 21,       // d8-d15
  kPersShift = 29,
  kOShift = 32,
  kNShift = 36,
  kHasProShift = 48,
  kHasEpiShift = 49,
  kPSShift = 50,
  kAposShift = 54,
  kTailLenShift = 56,
  kTailSignedShift = 60,
  kLayoutShift = 48, // PARTIAL: index of the layout descriptor
  // Escape descriptors: CIE augmentations S (signal frame), G (MTE-tagged
  // frame) and N (non-hint PAuth instructions), and the program's offset in
  // the escape area.
  kEscSignalShift = 8,
  kEscMteShift = 9,
  kEscNonHintShift = 10,
  kEscOffsetShift = 32,
};

struct Slots {
  uint32_t mask;
  int32_t off[kNumCsr];
  void set(int r, int32_t o) {
    mask |= 1u << r;
    off[r] = o;
  }
};

// LLVM: consecutive 8-byte slots downward from CFA - top: x19..x28 (plus x30
// and x29 when there is no frame record), then the x30/x29 frame record, then
// d8..d15. If the total is odd, the last register of the odd-sized group sits
// below an 8-byte hole.
inline void llvmSlots(uint32_t regs, int top, bool frameRecord, Slots &s) {
  int order[kNumCsr], n = 0;
  for (int i = 0; i < 10; ++i)
    if (regs >> i & 1)
      order[n++] = i;
  if (!frameRecord) {
    if (regs >> kX30 & 1)
      order[n++] = kX30;
    if (regs >> kX29 & 1)
      order[n++] = kX29;
  }
  int npool = n;
  if (frameRecord) {
    order[n++] = kX30;
    order[n++] = kX29;
  }
  int drStart = n;
  for (int i = 12; i < kNumCsr; ++i)
    if (regs >> i & 1)
      order[n++] = i;
  int ndr = n - drStart;
  int hole = -1;
  if (n % 2)
    hole = npool % 2 ? order[npool - 1] : ndr % 2 ? order[n - 1] : -1;
  int o = -top;
  for (int k = 0; k < n; ++k) {
    if (order[k] == hole)
      o -= 8;
    o -= 8;
    s.set(order[k], o);
  }
}

// GCC: upward from base, the GPRs with a frame record first, otherwise x30
// first and x29 last; then d8..d15 from the next 16-byte boundary, except that
// the last FP register fills the alignment gap when it would start 16-byte
// aligned (aarch64_layout_frame).
inline void gccSlots(uint32_t regs, int base, bool frameRecord, Slots &s) {
  int order[12], n = 0;
  if (frameRecord) {
    order[n++] = kX29;
    order[n++] = kX30;
  } else if (regs >> kX30 & 1) {
    order[n++] = kX30;
  }
  for (int i = 0; i < 10; ++i)
    order[n++] = i;
  if (!frameRecord)
    order[n++] = kX29;
  int o = base;
  for (int k = 0; k < n; ++k)
    if (regs >> order[k] & 1) {
      s.set(order[k], o);
      o += 8;
    }
  int intEnd = o;
  o = base + ((o - base + 15) & ~15);
  bool gap = o != intEnd;
  int fps[8], nf = 0;
  for (int i = 12; i < kNumCsr; ++i)
    if (regs >> i & 1)
      fps[nf++] = i;
  for (int k = 0; k < nf; ++k) {
    if (k == nf - 1 && gap && (o - base) % 16 == 0) {
      s.set(fps[k], intEnd);
      break;
    }
    s.set(fps[k], o);
    o += 8;
  }
}

// GCC's order with the save area ending top bytes below the CFA (stack
// protector frames, aarch64_save_regs_above_locals_p).
inline void gccTopSlots(uint32_t regs, int top, bool frameRecord, Slots &s) {
  Slots t;
  t.mask = 0;
  gccSlots(regs, 0, frameRecord, t);
  if (!t.mask)
    return;
  int mx = -0x7fffffff;
  for (int r = 0; r < kNumCsr; ++r)
    if ((t.mask >> r & 1) && t.off[r] > mx)
      mx = t.off[r];
  int size = (mx + 8 + 15) & ~15;
  for (int r = 0; r < kNumCsr; ++r)
    if (t.mask >> r & 1)
      s.set(r, t.off[r] - size - top);
}

// A descriptor's frame shape, slots and prologue/epilogue lengths.
struct Shape {
  unsigned mode;
  Conv conv;
  bool partial, fp, ra, withPC, scs, tailSigned, frameRecord;
  unsigned N, O;
  uint32_t layRegs; // registers with slots
  uint32_t saved;   // registers saved in the body
  Slots slots;
  int area; // bytes from the lowest slot to the CFA
  // Canonical lengths for this shape, with PS computed from the effective
  // lead-in.
  unsigned canonApos, canonPS, canonTailLen;
  // Effective lengths (overrides applied); EpiLen and akind are always
  // canonical.
  bool hasPro, hasEpi, akind;
  unsigned apos, PS, EpiLen, TailLen;
};

inline int popcount(uint32_t v) { return __builtin_popcount(v); }

inline void computeLengths(uint64_t desc, Shape &s);

// Decodes the frame shape of a NULL, FP, SP or PARTIAL descriptor. layout is
// the layout descriptor for PARTIAL descriptors and ignored otherwise.
// Returns false for other modes and invalid descriptors.
inline bool decodeShape(uint64_t desc, uint64_t layout, Shape &s) {
  s.mode = desc & 7;
  if (s.mode == kModeEscape || s.mode > kModePartialSp)
    return false;
  s.partial = s.mode == kModePartialFp || s.mode == kModePartialSp;
  s.fp = s.mode == kModeFp || s.mode == kModePartialFp;
  unsigned raField = (desc >> kRaShift) & 7;
  s.ra = raField != 0;
  s.withPC = raField >= 3;
  // PARTIAL descriptors take the convention from their layout descriptor and
  // use their own field only for the shadow call stack (0 or 1).
  unsigned ownConv = (desc >> kConvShift) & 3;
  s.scs = ownConv == 1;
  s.N = (desc >> kNShift) & 0xfff;
  uint64_t lay = s.partial ? layout : desc;
  unsigned layConv = (lay >> kConvShift) & 3;
  s.conv = layConv < 2 ? kLlvm : layConv == 2 ? kGcc : kGccTop;
  s.O = (lay >> kOShift) & 0xf;
  s.layRegs = (uint32_t)((lay >> kRegsShift) & 0xfff) |
              (uint32_t)((lay >> kDRegsShift) & 0xff) << 12;
  uint32_t ownRegs = (uint32_t)((desc >> kRegsShift) & 0xfff) |
                     (uint32_t)((desc >> kDRegsShift) & 0xff) << 12;
  s.frameRecord = (lay >> kFrameRecordShift) & 1;
  if ((s.frameRecord && (s.layRegs & kRecordBits) != kRecordBits) ||
      (s.partial && (((desc >> kFrameRecordShift) & 1) || ownConv > 1 ||
                     (s.scs && s.conv != kLlvm) || (ownRegs & ~s.layRegs))))
    return false;
  s.slots.mask = 0;
  if (s.mode != kModeNull) {
    if (s.conv == kLlvm)
      llvmSlots(s.layRegs, 16 * (int)s.O, s.frameRecord, s.slots);
    else if (s.conv == kGccTop)
      gccTopSlots(s.layRegs, 16 * (int)s.O, s.frameRecord, s.slots);
    else
      gccSlots(s.layRegs, -16 * (int)s.N + 16 * (int)s.O, s.frameRecord,
               s.slots);
    if (s.fp && !(s.slots.mask >> kX29 & 1))
      return false;
  }
  s.saved = s.mode == kModeNull ? 0 : s.partial ? ownRegs : s.layRegs;
  s.area = 0;
  for (int r = 0; r < kNumCsr; ++r)
    if ((s.slots.mask >> r & 1) && -s.slots.off[r] > s.area)
      s.area = -s.slots.off[r];
  computeLengths(desc, s);
  return true;
}

// Recomputes the lengths of a decoded shape from desc's prologue and epilogue
// fields, which an encoder may change without changing the shape.
inline void computeLengths(uint64_t desc, Shape &s) {
  // Prologue and epilogue lengths. The lead-in is the signing instructions
  // (PAuth_LR signs with pacm; paciasp); the shadow call stack push follows
  // it. The tail is the shadow call stack pop, then before the return
  // PAuth_LR materializes the signing address (adrp, add) and issues pacm, and
  // autiasp follows unless the return authenticates.
  bool windows = !s.partial && s.mode != kModeNull;
  s.hasPro = windows && ((desc >> kHasProShift) & 1);
  s.hasEpi = windows && ((desc >> kHasEpiShift) & 1);
  s.tailSigned = s.hasEpi && ((desc >> kTailSignedShift) & 1);
  unsigned apx = s.hasPro ? (desc >> kAposShift) & 3 : 0;
  unsigned psx = s.hasPro ? (desc >> kPSShift) & 0xf : 0;
  unsigned tailLenOverride = s.hasEpi ? (desc >> kTailLenShift) & 0xf : 0;
  int area = s.area;
  unsigned scs = s.scs ? 1 : 0;
  s.canonApos = s.ra ? (s.withPC ? 2 : 1) : 0;
  s.apos = apx ? apx - 1 : s.canonApos;
  unsigned auth = s.ra ? (s.withPC ? 3 : 0) + (s.tailSigned ? 0 : 1) : 0;
  s.canonTailLen = 1 + auth + scs;
  s.TailLen = tailLenOverride ? tailLenOverride : s.canonTailLen;
  if (s.conv == kLlvm) {
    uint32_t pool = s.layRegs & 0x3ff;
    if (!s.frameRecord)
      pool |= s.layRegs & kRecordBits;
    unsigned g = (popcount(pool) + 1) / 2 + (s.frameRecord ? 1 : 0) +
                 (popcount(s.layRegs >> 12) + 1) / 2;
    bool pre = 16 * (int)s.N == area && g > 0;
    s.akind = false;
    s.canonPS = s.apos + scs + (pre ? 0 : 1) + g + (s.fp ? 1 : 0);
    s.EpiLen = s.fp ? (pre ? g : g + 1) : 0;
  } else {
    // Only the allocating store is canonical; with a pre-index frame record
    // store, the record is in its slot from then on.
    bool pre = s.conv == kGccTop
                   ? s.frameRecord && s.O == 0 && !s.fp &&
                         16 * (int)s.N == area && area <= 512
                   : s.frameRecord && s.O == 0 && 16 * (int)s.N <= 504;
    s.akind = pre;
    s.canonPS = s.apos + scs + 1;
    s.EpiLen = 0;
  }
  s.PS = psx ? psx : s.canonPS;
  if (!s.hasPro)
    s.PS = 0;
  if (!s.hasEpi)
    s.EpiLen = s.TailLen = 0;
}

// The frame at instruction i of an n-instruction region.
struct Frame {
  bool cfaFp = false; // CFA = x29 + cfaOff, else sp + cfaOff
  int32_t cfaOff = 0;
  uint32_t saved = 0; // registers saved at slots.off[r] from the CFA
  bool raSigned = false;
  bool x18 = false; // x18 = the caller's x18 + 8 (shadow call stack pushed)
};

inline Frame frameAt(const Shape &s, unsigned n, unsigned i) {
  Frame f;
  f.raSigned = s.ra;
  f.x18 = s.scs;
  f.saved = s.saved;
  f.cfaFp = false;
  f.cfaOff = 0;
  if (s.mode != kModeNull) {
    if (s.fp) {
      f.cfaFp = true;
      f.cfaOff = -s.slots.off[kX29];
    } else {
      f.cfaOff = 16 * (int32_t)s.N;
    }
  }
  if (i < s.PS) {
    f.cfaFp = false;
    if (i <= s.apos) {
      f.cfaOff = 0;
      f.saved = 0;
      f.raSigned = s.ra && i >= s.apos;
      f.x18 = false;
    } else if (s.scs && i == s.apos + 1) {
      f.cfaOff = 0;
      f.saved = 0;
    } else {
      f.cfaOff = 16 * (int32_t)s.N;
      f.saved = s.akind ? (s.slots.mask & kRecordBits) : 0;
    }
    return f;
  }
  if (s.TailLen && i + s.TailLen >= n) {
    f.cfaFp = false;
    f.cfaOff = 0;
    // Signed up to and including the authentication; instructions after it
    // (a tail call's argument moves) are unsigned.
    unsigned signedLen = (s.scs ? 1 : 0) + (s.withPC ? 4 : 1);
    f.raSigned = s.ra && (s.tailSigned || i - (n - s.TailLen) < signedLen);
    // At the shadow call stack pop the saved registers are still described
    // in their slots.
    f.x18 = s.scs && i == n - s.TailLen;
    if (!f.x18)
      f.saved = 0;
    return f;
  }
  if (s.EpiLen && i + s.TailLen + s.EpiLen >= n) {
    f.cfaFp = false;
    f.cfaOff = 16 * (int32_t)s.N;
  }
  return f;
}

} // namespace aarch64_compact_unwind
} // namespace libunwind

#endif // __AARCH64COMPACTUNWIND_HPP__
