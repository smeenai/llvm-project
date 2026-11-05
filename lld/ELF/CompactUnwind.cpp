//===- CompactUnwind.cpp --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// AArch64 compact unwind tables (-z compact-unwind), which replace .eh_frame
// and are written into .eh_frame_hdr (see
// llvm/docs/AArch64ELFCompactUnwind.md). Each live FDE's CFI is split into
// regions, each described by a 64-bit descriptor that is checked against every
// CFI row it covers. CFI that no descriptor can express is rewritten as an
// escape program, which the unwinder interprets as DWARF.
//
//===----------------------------------------------------------------------===//

#include "CompactUnwind.h"
#include "Config.h"
#include "EhFrame.h"
#include "InputFiles.h"
#include "InputSection.h"
#include "OutputSections.h"
#include "Symbols.h"
#include "SyntheticSections.h"
#include "Target.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/BinaryFormat/ELFCompactUnwind.h"
#include "llvm/DebugInfo/DWARF/LowLevel/DWARFCFIProgram.h"
#include "llvm/DebugInfo/DWARF/LowLevel/DWARFUnwindTable.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/Parallel.h"
#include <array>
#include <optional>

using namespace llvm;
using namespace llvm::dwarf;
using namespace llvm::support::endian;
using namespace lld;
using namespace lld::elf;

namespace cu = llvm::ELFCompactUnwind;
namespace a64 = llvm::ELFCompactUnwind::AArch64;

namespace {
constexpr unsigned kSP = 31, kFP = 29;
// Callee-saved register indices: x19-x28 are 0-9, x29 is 10, x30 is 11 and
// d8-d15 are 12-19.
using a64::FrameRecordCSRs;
using a64::NumCSRs;

int csrIndex(uint64_t reg) {
  if (reg >= 19 && reg <= 30)
    return reg - 19;
  if (reg >= 72 && reg <= 79)
    return reg - 72 + 12;
  return -1;
}

// The rules at an instruction as the CFI gives them: what a descriptor's frame
// says, plus the register offsets and the PAuth_LR modifier, which a
// descriptor keeps in its shape.
struct State : a64::Frame {
  // Signed with the PC as a second modifier (PAuth_LR).
  bool raPc = false;
  std::array<int32_t, NumCSRs> off{};

  // No frame: the CFA is SP and nothing is saved, apart from possibly the
  // shadow call stack push.
  bool isFrameless() const { return !CFAIsFP && CFAOffset == 0 && Saved == 0; }
  bool operator==(const State &o) const {
    if (CFAIsFP != o.CFAIsFP || CFAOffset != o.CFAOffset ||
        RASigned != o.RASigned || raPc != o.raPc || X18 != o.X18 ||
        Saved != o.Saved)
      return false;
    for (uint32_t m = Saved; m; m &= m - 1)
      if (off[llvm::countr_zero(m)] != o.off[llvm::countr_zero(m)])
        return false;
    return true;
  }
  bool operator!=(const State &o) const { return !(*this == o); }
};

State toState(const a64::Shape &s, const a64::Frame &f) {
  State st;
  static_cast<a64::Frame &>(st) = f;
  st.raPc = f.RASigned && s.WithPC;
  for (uint32_t m = f.Saved; m; m &= m - 1)
    st.off[llvm::countr_zero(m)] = s.Slots.Offsets[llvm::countr_zero(m)];
  return st;
}

// A descriptor without its personality, and what it decodes to.
struct Desc {
  uint64_t bits = 0;
  a64::Shape shape;
};

// Bits 3-4: 0 LLVM, 1 LLVM with a shadow call stack, 2 GCC sp-anchored, 3 GCC
// CFA-anchored.
uint64_t convBits(a64::Convention c, bool scs) {
  return uint64_t(c == a64::ConvLLVM  ? (scs ? 1 : 0)
                  : c == a64::ConvGCC ? 2
                                      : 3)
         << a64::ConvShift;
}

// The x19-x30 and d8-d15 masks and the frame record bit.
uint64_t regBits(uint32_t r, bool frameRecord) {
  return uint64_t(r & 0xfff) << a64::RegsShift |
         uint64_t(frameRecord) << a64::FrameRecordShift |
         uint64_t(r >> 12 & 0xff) << a64::DRegsShift;
}

uint64_t withN(uint64_t bits, unsigned N) {
  return (bits & ~(uint64_t(0xfff) << a64::NShift)) | uint64_t(N)
                                                          << a64::NShift;
}

struct Fn {
  unsigned n = 0;
  // The CIE's B key and non-hint PAuth instruction augmentations, which pick
  // the RA scheme field.
  bool bKey = false, nonHint = false;
  SmallVector<unsigned, 8> starts;
  SmallVector<State, 8> states;
  // states[k] equals states[j] iff ids[k] == ids[j] (filled by encodeAsync).
  SmallVector<uint32_t, 8> ids;
  SmallVector<unsigned, 0> calls;
  // bodyCandidates' results, shared with the functions encoded alongside.
  StringMap<SmallVector<Desc, 4>> *candidates = nullptr;

  const State &at(unsigned i) const {
    return states[llvm::upper_bound(starts, i) - starts.begin() - 1];
  }
};

// 1/2: A/B key; 3/4: A/B with the PC (PACM hint); 5/6: A/B with the PC
// (non-hint instructions).
uint64_t raField(const Fn &fn, const State &st) {
  if (!st.RASigned)
    return 0;
  return (st.raPc ? (fn.nonHint ? 5 : 3) : 1) + (fn.bKey ? 1 : 0);
}

// Interprets the CIE and FDE instructions over a function of the given size
// with LLVM's CFI interpreter. Fills fn with the normalized rows when every row
// is expressible; otherwise sets why. escapable is cleared for programs the
// unwinder can't run from a copy.
struct Interp {
  StringRef why;
  bool escapable = true;

  // cieRow is the row the CIE's instructions set up, if the interpreter
  // accepts them.
  void run(const dwarf::CFIProgram &cie,
           const std::optional<dwarf::UnwindRow> &cieRow,
           const dwarf::CFIProgram &fde, uint64_t size, Fn &fn) {
    fn.n = size / 4;
    // What the rows don't show: positions that aren't relative to the
    // function, the argument size, and PAuth_LR negations.
    for (const dwarf::CFIProgram *p : {&cie, &fde}) {
      for (const dwarf::CFIProgram::Instruction &inst : *p) {
        switch (inst.Opcode) {
        case DW_CFA_advance_loc:
        case DW_CFA_advance_loc1:
        case DW_CFA_advance_loc2:
        case DW_CFA_advance_loc4:
          if (p == &cie)
            fail("cie_advance");
          break;
        case DW_CFA_set_loc:
          return unescapable("set_loc");
        case DW_CFA_GNU_args_size:
          if (inst.Ops[0])
            fail("args_size");
          break;
        case DW_CFA_AARCH64_negate_ra_state_with_pc:
          fail("pauth_lr_negate");
          break;
        case DW_CFA_AARCH64_set_ra_state:
          if (inst.Ops[0] > 2)
            return unescapable("malformed");
          break;
        }
      }
    }
    if (!why.empty())
      return;
    // Programs the interpreter rejects, such as GCC's DW_CFA_def_cfa_offset
    // after a CFA expression, become escapes, which the unwinder runs as it
    // would the FDE.
    // Each row holds from its address to the next row's, and the last to the
    // end of the function.
    std::optional<dwarf::UnwindRow> prev;
    auto add = [&](uint64_t end) {
      uint64_t loc = prev->getAddress();
      if (end <= loc || loc >= size || !why.empty())
        return;
      if (loc % 4)
        return fail("unaligned_row");
      State st;
      if (!normalize(*prev, st) ||
          (!fn.states.empty() && fn.states.back() == st))
        return;
      fn.starts.push_back(loc / 4);
      fn.states.push_back(st);
    };
    if (!cieRow)
      return fail("cfi_rule");
    dwarf::UnwindRow row = *cieRow;
    dwarf::RegisterLocations init = row.getRegisterLocations();
    row.setAddress(0);
    if (Error e =
            dwarf::parseRows(fde, row, &init, [&](const dwarf::UnwindRow &r) {
              if (prev)
                add(r.getAddress());
              prev = r;
            })) {
      consumeError(std::move(e));
      return fail("cfi_rule");
    }
    if (prev)
      add(row.getAddress());
    prev = row;
    add(size);
  }

private:
  void fail(StringRef w) {
    if (why.empty())
      why = w;
  }
  void unescapable(StringRef w) {
    escapable = false;
    fail(w);
  }
  bool normalize(const dwarf::UnwindRow &row, State &st) {
    using dwarf::UnwindLocation;
    const UnwindLocation &cfa = row.getCFAValue();
    if (cfa.getLocation() == UnwindLocation::DWARFExpr) {
      fail("cfa_expression");
      return false;
    }
    if (cfa.getLocation() == UnwindLocation::RegPlusOffset) {
      if (cfa.getRegister() != kSP && cfa.getRegister() != kFP) {
        fail("cfa_reg");
        return false;
      }
      st.CFAIsFP = cfa.getRegister() == kFP;
      st.CFAOffset = cfa.getOffset();
    }
    const dwarf::RegisterLocations &locs = row.getRegisterLocations();
    for (const auto &[reg, loc] : locs) {
      UnwindLocation::Location kind = loc.getLocation();
      if (kind == UnwindLocation::Same || kind == UnwindLocation::Unspecified)
        continue;
      // The return address signing state (0: not signed, 1: signed, 2:
      // signed with the PC; 3, from negating a PC-signed state, can't be
      // expressed).
      if (reg == 34 && kind == UnwindLocation::Constant) {
        if (loc.getConstant() > 2) {
          fail("pauth_lr_negate");
          return false;
        }
        st.RASigned = loc.getConstant() != 0;
        st.raPc = loc.getConstant() == 2;
        continue;
      }
      // The shadow call stack push: x18 = DW_OP_breg18 -8.
      if (reg == 18 && kind == UnwindLocation::DWARFExpr &&
          !loc.getDereference()) {
        std::optional<DWARFExpression> expr = loc.getDWARFExpressionBytes();
        auto op = expr->begin();
        if (op != expr->end() && std::next(op) == expr->end() &&
            op->getCode() == DW_OP_breg18 &&
            int64_t(op->getRawOperand(0)) == -8) {
          st.X18 = true;
          continue;
        }
      }
      int ci = csrIndex(reg);
      if (ci >= 0) {
        if (kind == UnwindLocation::CFAPlusOffset && loc.getDereference()) {
          st.Saved |= 1u << ci;
          st.off[ci] = loc.getOffset();
          continue;
        }
        fail(kind == UnwindLocation::Undefined ? "undefined_csr" : "reg_rule");
        return false;
      }
      if (kind != UnwindLocation::Undefined) {
        fail("saves_non_aapcs_reg");
        return false;
      }
    }
    return true;
  }
};

//===----------------------------------------------------------------------===//
// Region search (descriptor candidates, async regions, sync runs)
//===----------------------------------------------------------------------===//

// The descriptors whose body is st, smallest O first, then by convention. A
// state that more than one convention describes gets LLVM's, which Clang's
// other frames use, so that neighbouring regions' descriptors can match.
const SmallVector<Desc, 4> &bodyCandidates(Fn &fn, const State &st) {
  // The candidates depend on the state and the RA scheme.
  uint64_t ra = raField(fn, st) << a64::RAShift;
  SmallString<64> key;
  auto put = [&](auto v) {
    const char *p = reinterpret_cast<const char *>(&v);
    key.append(p, p + sizeof(v));
  };
  put(uint8_t(st.CFAIsFP | st.RASigned << 1 | st.raPc << 2 | st.X18 << 3));
  put(st.CFAOffset);
  put(st.Saved);
  for (uint32_t m = st.Saved; m; m &= m - 1)
    put(st.off[llvm::countr_zero(m)]);
  put(ra);
  auto [it, inserted] = fn.candidates->try_emplace(key);
  SmallVector<Desc, 4> &out = it->second;
  if (!inserted)
    return out;
  auto add = [&](uint64_t bits) {
    Desc d{bits, {}};
    if (a64::decodeShape(bits, 0, d.shape) &&
        toState(d.shape, a64::frameAt(d.shape, 1, 0)) == st)
      out.push_back(d);
  };
  if (st.isFrameless()) {
    add(a64::ModeNull | convBits(a64::ConvLLVM, st.X18) | ra);
  } else {
    uint32_t regs = st.Saved;
    // A frame record has x29 just below x30, and d8-d15 sit below the
    // general registers in LLVM's convention and above them in GCC's.
    bool recordOk = (regs & FrameRecordCSRs) != FrameRecordCSRs ||
                    st.off[a64::CSRX29] == st.off[a64::CSRX30] - 8;
    int32_t gprLo = INT32_MAX, gprHi = INT32_MIN, fprLo = INT32_MAX,
            fprHi = INT32_MIN;
    for (uint32_t m = regs; m; m &= m - 1) {
      int r = llvm::countr_zero(m);
      int32_t &lo = r < 12 ? gprLo : fprLo, &hi = r < 12 ? gprHi : fprHi;
      lo = std::min(lo, st.off[r]);
      hi = std::max(hi, st.off[r]);
    }
    bool both = gprLo != INT32_MAX && fprLo != INT32_MAX;
    bool fprBelow = both && fprHi < gprLo, fprAbove = both && fprLo > gprHi;
    for (bool fr : {true, false}) {
      // Frame-pointer frames need the frame record.
      if (fr ? (regs & FrameRecordCSRs) != FrameRecordCSRs || !recordOk
             : st.CFAIsFP)
        continue;
      for (a64::Convention conv :
           {a64::ConvLLVM, a64::ConvGCC, a64::ConvGCCTop}) {
        // The shadow call stack only exists with the LLVM convention.
        if (conv == a64::ConvLLVM ? fprAbove : st.X18 || fprBelow)
          continue;
        unsigned N = 0;
        if (!st.CFAIsFP || conv == a64::ConvGCC) {
          if (st.CFAOffset % 16 || st.CFAOffset < 0 ||
              st.CFAOffset / 16 > int(a64::MaxN))
            continue;
          N = st.CFAOffset / 16;
        }
        auto bitsFor = [&](unsigned O) {
          return (st.CFAIsFP ? a64::ModeFP : a64::ModeSP) |
                 convBits(conv, st.X18) | ra | regBits(regs, fr) |
                 uint64_t(O) << a64::OShift | uint64_t(N) << a64::NShift;
        };
        // O moves every slot by the same 16·O bytes (up for GCC's
        // sp-anchored convention, down for the others), so any saved
        // register's offset gives it.
        a64::Shape s0;
        if (!a64::decodeShape(bitsFor(0), 0, s0))
          continue;
        int o = 0;
        if (uint32_t m = regs & s0.Slots.Mask) {
          int r = llvm::countr_zero(m);
          int delta = conv == a64::ConvGCC ? st.off[r] - s0.Slots.Offsets[r]
                                           : s0.Slots.Offsets[r] - st.off[r];
          if (delta < 0 || delta % 16)
            continue;
          o = delta / 16;
        }
        if (o > int(st.CFAIsFP && conv == a64::ConvGCC ? 0 : a64::MaxO))
          continue;
        add(bitsFor(o));
      }
    }
    llvm::stable_sort(out, [](const Desc &a, const Desc &b) {
      return std::make_pair(a.shape.O, a.shape.Conv) <
             std::make_pair(b.shape.O, b.shape.Conv);
    });
  }
  return out;
}

// A run of one CFI state within a region, in instructions from its start.
struct Seg {
  unsigned s, e;
  const State *st;
  uint32_t id;
};

// The prologue, epilogue window and tail lengths of an n-instruction region
// whose body is segs[f..l], and the sp-relative CFA offset the windows share,
// if any.
struct Windows {
  unsigned PS = 0, EpiLen = 0, TailLen = 0;
  std::optional<int32_t> spOff;
};

std::optional<Windows> deriveWindows(ArrayRef<Seg> segs, size_t f, size_t l,
                                     unsigned n) {
  Windows w;
  w.PS = segs[f].s;
  if (l + 1 < segs.size()) {
    // The tail starts where the CFA returns to SP for good. Registers may
    // still be described in their slots there (the shadow call stack pop);
    // the check against the decoded states decides. The epilogue window lies
    // between the body and the tail.
    size_t t = SIZE_MAX;
    for (size_t k = l + 1; k < segs.size(); ++k) {
      bool allNull = true;
      for (size_t j = k; j < segs.size(); ++j)
        allNull &= !segs[j].st->CFAIsFP && segs[j].st->CFAOffset == 0;
      if (allNull) {
        t = k;
        break;
      }
    }
    if (t == SIZE_MAX)
      return std::nullopt;
    w.TailLen = n - segs[t].s;
    w.EpiLen = segs[t].s - segs[l].e;
    if (w.TailLen > a64::MaxTailLen || w.EpiLen > a64::MaxEpiLen)
      return std::nullopt;
  }
  if (w.PS > 63)
    return std::nullopt;
  // SP-based CFA offsets outside the body must agree.
  for (size_t k = 0; k < segs.size(); ++k) {
    if (k >= f && k <= l)
      continue;
    const State &st = *segs[k].st;
    if (!st.CFAIsFP && st.CFAOffset) {
      if (w.spOff && *w.spOff != st.CFAOffset)
        return std::nullopt;
      w.spOff = st.CFAOffset;
    }
  }
  return w;
}

std::optional<Desc> tryRegion(Fn &fn, unsigned a, unsigned b) {
  SmallVector<Seg, 8> segs;
  size_t lo = llvm::upper_bound(fn.starts, a) - fn.starts.begin() - 1;
  size_t hi = llvm::lower_bound(fn.starts, b) - fn.starts.begin();
  for (size_t k = lo; k < hi; ++k) {
    unsigned s0 = std::max(fn.starts[k], a);
    unsigned e0 =
        std::min(k + 1 < fn.starts.size() ? fn.starts[k + 1] : fn.n, b);
    if (e0 > s0)
      segs.push_back({s0 - a, e0 - a, &fn.states[k], fn.ids[k]});
  }
  unsigned n = b - a;
  // The body is the frame-pointer state if there is one (prologue and
  // epilogue windows are sp-based), else the framed state saving the most
  // registers.
  const Seg *body = &segs[0];
  for (const Seg &sg : segs) {
    auto rank = [](const State &st) {
      return std::make_tuple(st.CFAIsFP, !st.isFrameless(),
                             llvm::popcount(st.Saved));
    };
    if (rank(*sg.st) > rank(*body->st))
      body = &sg;
  }
  size_t f = SIZE_MAX, l = 0;
  for (size_t k = 0; k < segs.size(); ++k)
    if (segs[k].id == body->id) {
      f = std::min(f, k);
      l = k;
    }
  for (size_t k = f; k <= l; ++k)
    if (segs[k].id != body->id)
      return std::nullopt;
  std::optional<Windows> w = deriveWindows(segs, f, l, n);
  if (!w)
    return std::nullopt;
  unsigned PS = w->PS, EpiLen = w->EpiLen, TailLen = w->TailLen;
  std::optional<int32_t> nsp = w->spOff;
  for (const Desc &c : bodyCandidates(fn, *body->st)) {
    if ((PS || TailLen) && c.shape.Mode == a64::ModeNull)
      continue;
    if (c.shape.Mode == a64::ModeSP && nsp && *nsp != 16 * int(c.shape.N))
      continue;
    Desc d = c;
    if (c.shape.FP && c.shape.Conv == a64::ConvGCC) {
      if (nsp && *nsp != 16 * int(c.shape.N))
        continue;
    } else if (c.shape.FP) {
      int nfp = nsp ? *nsp / 16 : 0;
      if (nfp < 0 || nfp > int(a64::MaxN))
        continue;
      d.bits = withN(d.bits, (PS || EpiLen) ? nfp : 0);
    }
    d.bits |= uint64_t(PS != 0) << a64::HasPrologueShift |
              uint64_t(TailLen != 0) << a64::HasEpilogueShift;
    // The slots, akind and EpiLen follow from the frame shape alone.
    a64::Shape &s = d.shape;
    if (!a64::decodeShape(d.bits, 0, s) || (TailLen && EpiLen != s.EpiLen))
      continue;
    // The tail is signed to its end only if the CFI says so at the last
    // instruction, which matters only when the tail outlasts its signed
    // prefix.
    unsigned signedLen = (s.SCS ? 1 : 0) + (s.WithPC ? 4 : 1);
    if (TailLen && s.RA && signedLen < TailLen && fn.at(a + n - 1).RASigned)
      d.bits |= uint64_t(1) << a64::TailSignedShift;
    // The lead-in ends where the return address becomes signed, or just
    // before the shadow call stack push; failing both, it is the frameless
    // start of the window less the allocating store. A frame-pointer window of
    // N = 0 keeps the CFA at sp, so it looks frameless too and the lead-in is
    // empty.
    if (PS) {
      unsigned p = 0;
      if (s.RA || s.SCS) {
        while (p < PS && !(s.RA ? fn.at(a + p).RASigned : fn.at(a + p).X18))
          ++p;
        if (!s.RA && p)
          --p;
      } else if (!(s.FP && s.N == 0)) {
        while (p < PS && fn.at(a + p).isFrameless())
          ++p;
        p = p ? p - 1 : 0;
      }
      if (p >= PS || p > 2)
        continue;
      if (p != s.CanonApos)
        d.bits |= uint64_t(p + 1) << a64::AposShift;
    }
    // Store the prologue and tail lengths that differ from the canonical
    // ones.
    a64::computeLengths(d.bits, s);
    if (PS && PS != s.CanonPS) {
      if (PS > a64::MaxPS)
        continue;
      d.bits |= uint64_t(PS) << a64::PSShift;
    }
    if (TailLen && TailLen != s.CanonTailLen) {
      if (TailLen > a64::MaxTailLen)
        continue;
      d.bits |= uint64_t(TailLen) << a64::TailLenShift;
    }
    a64::computeLengths(d.bits, s);
    // Both the CFI and the decoded rules only change at these points, so
    // checking them checks every instruction.
    SmallVector<unsigned, 16> pts = a64::frameChanges(s, n);
    for (const Seg &sg : segs)
      pts.push_back(sg.s);
    bool ok = true;
    for (unsigned pt : pts)
      if (toState(s, a64::frameAt(s, n, pt)) != fn.at(a + pt)) {
        ok = false;
        break;
      }
    if (!ok)
      continue;
    return d;
  }
  return std::nullopt;
}

// What the table needs of a region's descriptor.
struct RegionDesc {
  uint64_t bits;
  uint8_t ps, tailLen;
  RegionDesc(const Desc &d)
      : bits(d.bits), ps(d.shape.PS), tailLen(d.shape.TailLen) {}
};
using Regions = SmallVector<std::pair<unsigned, RegionDesc>, 2>;

// One region per frame episode (a run of framed states between frameless
// ones), where possible.
//
// An episode's region starts with its prologue's lead-in: the frameless run
// before the frame when it fits (up to three instructions, two before the
// signed state and then the allocating store, plus the shadow call stack push;
// or the whole run, as a prologue window of N = 0, for a frame-pointer body
// with no sp-based frame), else the canonical lead-in after a frameless region.
// A frame that was already seen (code after an early return) is re-entered
// with no prologue.
//
// The previous region can only end in a tail if its frame was last sp-based
// (an SP body or an epilogue window). The tail then takes the
// frameless run up to the next region's canonical lead-in if that fits, else
// just the signed part of the run and the return, leaving the rest to a
// frameless region. Otherwise the run goes to the next region's lead-in.
//
// An episode that doesn't fit one region (GCC records each prologue save, so
// every intermediate state needs its own region) gets a region per state.
bool encodeAsync(Fn &fn, Regions &out) {
  size_t S = fn.states.size();
  fn.ids.resize(S);
  for (size_t k = 0; k < S; ++k) {
    size_t j = 0;
    while (fn.states[j] != fn.states[k])
      ++j;
    fn.ids[k] = j;
  }
  auto segEnd = [&](size_t k) { return k + 1 < S ? fn.starts[k + 1] : fn.n; };

  SmallVector<unsigned, 8> bounds = {0};
  auto bound = [&](unsigned b) {
    if (b > bounds.back() && b < fn.n)
      bounds.push_back(b);
  };
  SmallVector<uint32_t, 4> seen;
  bool havePrev = false;
  unsigned prevEnd = 0;
  size_t prevEndSeg = 0;
  for (size_t k = 0; k < S;) {
    if (fn.states[k].isFrameless()) {
      ++k;
      continue;
    }
    size_t kb = k;
    while (k < S && !fn.states[k].isFrameless())
      ++k;
    unsigned F = fn.starts[kb];
    const State &st = fn.states[kb];
    unsigned scs = st.X18 ? 1 : 0;
    unsigned canonLead = llvm::is_contained(seen, fn.ids[kb])
                             ? 0
                             : 1 + scs + (st.RASigned ? (st.raPc ? 2 : 1) : 0);
    unsigned P;
    const State *last = havePrev ? &fn.states[prevEndSeg - 1] : nullptr;
    if (last && !last->CFAIsFP && !bodyCandidates(fn, *last).empty()) {
      unsigned minTail = prevEnd;
      for (size_t j = prevEndSeg; j < kb && fn.states[j].RASigned; ++j)
        minTail = segEnd(j);
      minTail = std::min(minTail + 1, F);
      P = std::max(F >= canonLead ? F - canonLead : 0, minTail);
      if (P - prevEnd > a64::MaxTailLen)
        bound(minTail);
    } else {
      unsigned maxLead = 3 + scs;
      bool spFrame = false;
      for (size_t i = kb; i < k; ++i)
        spFrame |= !fn.states[i].CFAIsFP && fn.states[i].CFAOffset != 0;
      if (st.CFAIsFP && !spFrame)
        maxLead = a64::MaxPS;
      size_t j = kb;
      while (j > 0 && fn.states[j - 1].isFrameless())
        --j;
      unsigned runStart = std::max(fn.starts[j], prevEnd);
      P = F - runStart <= maxLead
              ? runStart
              : std::max(F >= canonLead ? F - canonLead : 0, runStart);
      if (havePrev)
        bound(prevEnd);
    }
    bound(P);
    for (size_t j = kb; j < k; ++j)
      if (!llvm::is_contained(seen, fn.ids[j]))
        seen.push_back(fn.ids[j]);
    havePrev = true;
    prevEnd = segEnd(k - 1);
    prevEndSeg = k;
  }
  bounds.push_back(fn.n);

  for (size_t i = 0; i + 1 < bounds.size(); ++i) {
    unsigned lo = bounds[i], hi = bounds[i + 1];
    if (std::optional<Desc> d = tryRegion(fn, lo, hi)) {
      out.push_back({lo, *d});
      continue;
    }
    SmallVector<unsigned, 16> pts = {lo};
    for (unsigned s : fn.starts)
      if (s > lo && s < hi)
        pts.push_back(s);
    pts.push_back(hi);
    for (size_t p = 0; p + 1 < pts.size(); ++p) {
      std::optional<Desc> d = tryRegion(fn, pts[p], pts[p + 1]);
      if (!d)
        return false;
      out.push_back({pts[p], *d});
    }
  }
  return true;
}

// Whether fn's CFI is shaped like Clang's synchronous CFI: after possibly
// signing the return address and pushing to the shadow call stack, one row
// describes the whole frame, which then holds to the end. Such CFI is only
// meant to be right after that row, so its calls need not be found: calls
// before it go to prologue helpers, which never unwind.
bool describesFrameOnce(const Fn &fn) {
  bool framed = !fn.states[0].isFrameless();
  for (size_t k = 1; k < fn.states.size(); ++k) {
    const State &a = fn.states[k - 1], &b = fn.states[k];
    if (a == b)
      continue;
    if (framed || (a.RASigned && (!b.RASigned || a.raPc != b.raPc)) ||
        (a.X18 && !b.X18))
      return false;
    framed = !b.isFrameless();
  }
  return true;
}

bool encodeSync(Fn &fn, bool frameOnce, Regions &out) {
  SmallVector<std::pair<unsigned, const State *>, 8> cs;
  if (frameOnce) {
    cs.push_back({0, &fn.states.back()});
  } else if (!fn.calls.empty()) {
    for (unsigned c : fn.calls)
      cs.push_back({c, &fn.at(c)});
  } else {
    // No calls: the state covering the most instructions.
    SmallVector<std::pair<const State *, unsigned>, 8> lens;
    for (size_t k = 0; k < fn.starts.size(); ++k) {
      unsigned e = k + 1 < fn.starts.size() ? fn.starts[k + 1] : fn.n;
      auto it = llvm::find_if(
          lens, [&](auto &p) { return *p.first == fn.states[k]; });
      if (it == lens.end())
        lens.push_back({&fn.states[k], e - fn.starts[k]});
      else
        it->second += e - fn.starts[k];
    }
    auto best = lens.begin();
    for (auto it = lens.begin(); it != lens.end(); ++it)
      if (it->second > best->second)
        best = it;
    cs.push_back({0, best->first});
  }
  const State *prevSt = nullptr;
  for (auto [c, st] : cs) {
    if (prevSt && *prevSt == *st)
      continue;
    prevSt = st;
    const SmallVector<Desc, 4> &cands = bodyCandidates(fn, *st);
    if (cands.empty())
      return false;
    out.push_back({out.empty() ? 0 : c, cands[0]});
  }
  return true;
}

bool isCall(uint32_t insn) {
  return (insn & 0xfc000000) == 0x94000000 || // bl
         (insn & 0xfffffc1f) == 0xd63f0000 || // blr
         (insn & 0xfffffc1f) == 0xd63f081f || // blraaz
         (insn & 0xfffffc1f) == 0xd63f0c1f || // blrabz
         (insn & 0xfffffc00) == 0xd73f0800 || // blraa
         (insn & 0xfffffc00) == 0xd73f0c00;   // blrab
}

//===----------------------------------------------------------------------===//
// CIE/FDE parsing
//===----------------------------------------------------------------------===//

// Parses CFI instructions, which are in the target's byte order, into p.
bool parseCfi(ArrayRef<uint8_t> insns, bool isLE, dwarf::CFIProgram &p) {
  p.clear();
  DWARFDataExtractorSimple data(insns, isLE, /*AddressSize=*/8);
  uint64_t off = 0;
  return !errorToBool(p.parse(data, &off, insns.size()));
}

struct Cie {
  EhCie eh;
  std::optional<dwarf::CFIProgram> cfi;
  std::optional<dwarf::UnwindRow> initRow;
  StringRef err;
  bool bKey = false, signal = false, mte = false, nonHintPAuth = false;
  Symbol *pers = nullptr;
  int64_t persAddend = 0;
  // The personality routine's slot is its GOT entry, not the relocation's
  // target.
  bool persGot = false;
};

Cie readCie(const EhInputSection &sec, const EhSectionPiece &piece, bool isLE) {
  Cie c;
  c.eh = parseCie(sec, piece);
  c.cfi.emplace(c.eh.codeAlign, c.eh.dataAlign, Triple::aarch64);
  if (!parseCfi(c.eh.insns, isLE, *c.cfi)) {
    c.cfi.reset();
    c.err = "malformed";
  } else {
    dwarf::UnwindRow row;
    if (Error e = dwarf::parseRows(*c.cfi, row, nullptr,
                                   [](const dwarf::UnwindRow &) {}))
      consumeError(std::move(e));
    else
      c.initRow = row;
  }
  StringRef aug = c.eh.aug;
  c.bKey = aug.contains('B');
  c.signal = aug.contains('S');
  c.mte = aug.contains('G');
  c.nonHintPAuth = aug.contains('N');
  if (aug.contains('P')) {
    unsigned k = piece.firstRelocation;
    if (k != unsigned(-1) &&
        sec.rels[k].offset == piece.inputOff + c.eh.persOff) {
      const Relocation &r = sec.rels[k];
      c.pers = r.sym;
      c.persAddend = r.addend;
      c.persGot = !(c.eh.persEnc & DW_EH_PE_indirect) || r.expr == R_GOT_PC;
    } else {
      c.err = "personality_without_relocation";
    }
  }
  return c;
}

struct EncodedFn {
  StringRef why; // why the function needed an escape program or was dropped
  bool dropped = false;
  Symbol *sym = nullptr;
  int64_t addend = 0;
  uint32_t size = 0;
  const Cie *cie = nullptr;
  Symbol *lsdaSym = nullptr;
  int64_t lsdaAddend = 0;
  Regions regions;
  std::string escape;
};

// Writes the FDE's instructions as an escape program body, for the implied
// initial state: code alignment 4, data alignment -8, CFA = sp, nothing saved.
// Returns false for programs that can't be written that way.
bool canonicalEscape(const dwarf::CFIProgram &cie, const dwarf::CFIProgram &fde,
                     bool isLE, std::string &out) {
  // The CIE must establish exactly the implied state.
  bool cfa = false;
  for (const dwarf::CFIProgram::Instruction &inst : cie) {
    if (inst.Opcode == DW_CFA_nop)
      continue;
    if (inst.Opcode != DW_CFA_def_cfa || cfa || inst.Ops[0] != kSP ||
        inst.Ops[1] != 0)
      return false;
    cfa = true;
  }
  // Positions must be relative to the function start.
  if (!cfa || llvm::any_of(fde, [](const dwarf::CFIProgram::Instruction &i) {
        return i.Opcode == DW_CFA_set_loc;
      }))
    return false;
  std::string body;
  raw_string_ostream os(body);
  if (errorToBool(fde.encode(os, 4, -8, isLE, /*AddressSize=*/8)))
    return false;
  raw_string_ostream outOS(out);
  encodeULEB128(body.size(), outOS);
  outOS << body;
  return true;
}
} // namespace

//===----------------------------------------------------------------------===//
// Table
//===----------------------------------------------------------------------===//

struct CompactUnwindTable::Impl {
  Ctx &ctx;
  CompactUnwindKind kind;

  struct LDesc {
    uint64_t bits;
    bool foldable;
    bool hasEpi;
  };
  SmallVector<LDesc, 0> ldescs;
  DenseMap<uint64_t, uint32_t> ldescIds;

  struct Func {
    Symbol *sym;
    int64_t addend;
    uint32_t size;
    uint32_t pers; // 1-based index into pers, or 0
    Symbol *lsdaSym;
    int64_t lsdaAddend;
    SmallVector<std::pair<uint32_t, uint32_t>, 2> regions; // byte offset, ldesc
  };
  SmallVector<Func, 0> funcs;
  struct Pers {
    Symbol *sym;
    int64_t addend;
    bool got;
  };
  SmallVector<Pers, 0> pers;
  std::string escapes;
  uint32_t noInfo = 0;

  // Address-dependent layout.
  struct Entry {
    uint64_t va;
    uint32_t ldesc;
  };
  struct Page {
    uint64_t va;
    size_t first, count;
    SmallVector<uint32_t, 0> locals;
    size_t off;
  };
  // A table keyed by function start with 2-byte pc offsets.
  struct SideTable {
    uint8_t kind, payloadWidth;
    struct Row {
      uint64_t va;
      uint32_t func;
    };
    struct Page {
      uint64_t va;
      size_t first, count, off;
    };
    SmallVector<Row, 0> rows;
    SmallVector<Page, 0> pages;
    size_t flOff = 0;
  };
  SmallVector<Entry, 0> entries;
  SmallVector<Page, 0> pages;
  SmallVector<uint32_t, 0> globals;
  DenseMap<uint32_t, uint32_t> globalIdx;
  // LSDAs, for functions with a personality routine, and personality
  // indices, for functions whose personality doesn't fit in the descriptor.
  SideTable lsdaTable{cu::SideTableLSDA, 4, {}, {}},
      persTable{cu::SideTablePersonality, 2, {}, {}};
  uint64_t endVA = 0;
  size_t persOff = 0, dirOff = 0, gdescOff = 0, flOff = 0, sideOff = 0,
         escOff = 0, totalSize = 0;
  // The addresses the last layout was computed for, and the functions in
  // address order.
  SmallVector<uint64_t, 0> laidOutVAs;
  SmallVector<uint32_t, 0> funcOrder;

  SmallVector<SideTable *, 2> sideTables() {
    SmallVector<SideTable *, 2> v;
    for (SideTable *t : {&lsdaTable, &persTable})
      if (!t->rows.empty())
        v.push_back(t);
    return v;
  }

  Impl(Ctx &ctx) : ctx(ctx), kind(ctx.arg.zCompactUnwind) {}

  uint32_t getLDesc(uint64_t bits, bool foldable, bool hasEpi) {
    auto [it, inserted] = ldescIds.try_emplace(bits, ldescs.size());
    if (inserted)
      ldescs.push_back({bits, foldable, hasEpi});
    return it->second;
  }

  void encode();
  size_t layout(uint64_t hdrVA);
  void write(uint8_t *buf, uint64_t hdrVA);
};

void CompactUnwindTable::Impl::encode() {
  EhFrameSection &ehFrame = *ctx.in.ehFrame;
  bool sync = kind == CompactUnwindKind::Sync;

  // Parse CIEs once per input section.
  SmallVector<SmallVector<Cie, 0>, 0> cies(ehFrame.sections.size());
  struct Ref {
    size_t sec;
    const EhSectionPiece *fde;
  };
  SmallVector<Ref, 0> refs;
  for (size_t i = 0; i < ehFrame.sections.size(); ++i) {
    EhInputSection *sec = ehFrame.sections[i];
    if (!sec->isLive())
      continue;
    for (const EhSectionPiece &fde : sec->fdes)
      if (fde.outputOff != -1)
        refs.push_back({i, &fde});
  }
  parallelFor(0, ehFrame.sections.size(), [&](size_t i) {
    EhInputSection *sec = ehFrame.sections[i];
    for (const EhSectionPiece &cie : sec->cies)
      cies[i].push_back(readCie(*sec, cie, ctx.arg.isLE));
  });

  // Functions are encoded in chunks, each reusing a CFI program and the
  // candidates for the frame states it has seen.
  struct Scratch {
    std::optional<dwarf::CFIProgram> cfi;
    StringMap<SmallVector<Desc, 4>> candidates;
  };
  SmallVector<EncodedFn, 0> enc(refs.size());
  auto encodeOne = [&](size_t i, Scratch &scratch) {
    EhInputSection &sec = *ehFrame.sections[refs[i].sec];
    const EhSectionPiece &fde = *refs[i].fde;
    EncodedFn &out = enc[i];
    const Cie &cie = cies[refs[i].sec][fde.cie];
    out.cie = &cie;
    const Relocation &pcRel = sec.rels[fde.firstRelocation];
    out.sym = pcRel.sym;
    out.addend = pcRel.addend;
    if (!cie.err.empty()) {
      out.why = cie.err;
      out.dropped = true;
      return;
    }
    EhFde f = parseFde(sec, fde, cie.eh);
    if (cie.eh.lsdaEnc != DW_EH_PE_omit) {
      uint64_t fieldOff = fde.inputOff + f.lsdaOff;
      for (size_t k = fde.firstRelocation + 1;
           k < sec.rels.size() && sec.rels[k].offset < fde.inputOff + fde.size;
           ++k)
        if (sec.rels[k].offset == fieldOff) {
          out.lsdaSym = sec.rels[k].sym;
          out.lsdaAddend = sec.rels[k].addend;
        }
      if (!out.lsdaSym && f.lsda) {
        out.why = "lsda_without_relocation";
        out.dropped = true;
        return;
      }
    }
    // Zero-range FDEs describe no code (.eh_frame_hdr omits them too).
    if (f.pcRange == 0) {
      out.dropped = true;
      return;
    }
    if (f.pcRange > UINT32_MAX) {
      out.why = "malformed_fde";
      out.dropped = true;
      return;
    }
    uint64_t range = f.pcRange;
    out.size = range;
    std::optional<dwarf::CFIProgram> &cfi = scratch.cfi;
    if (!cfi || cfi->codeAlign() != cie.eh.codeAlign ||
        cfi->dataAlign() != cie.eh.dataAlign)
      cfi.emplace(cie.eh.codeAlign, cie.eh.dataAlign, Triple::aarch64);
    if (!parseCfi(f.insns, ctx.arg.isLE, *cfi)) {
      out.why = "malformed";
      out.dropped = true;
      return;
    }

    Fn fn;
    fn.candidates = &scratch.candidates;
    fn.bKey = cie.bKey;
    fn.nonHint = cie.nonHintPAuth;
    Interp interp;
    // Runs even when a descriptor is ruled out, to find the extent of the
    // instructions for an escape program.
    interp.run(*cie.cfi, cie.initRow, *cfi, range, fn);
    if (cie.eh.raReg != 30)
      interp.why = "ra_reg";
    else if (cie.signal || cie.mte)
      interp.why = cie.signal ? "signal_frame" : "mte_frame";
    else if (range % 4)
      interp.why = "odd_size";
    if (interp.why.empty()) {
      bool anyPc = false, anyPlain = false;
      for (const State &st : fn.states) {
        anyPc |= st.raPc;
        anyPlain |= st.RASigned && !st.raPc;
      }
      if (anyPc)
        interp.why = "pauth_lr";
      // Non-hint PAuth instructions always sign with the PC.
      else if (cie.nonHintPAuth && anyPlain)
        interp.why = "non_hint_pauth";
    }
    if (interp.why.empty()) {
      bool ok;
      if (sync) {
        bool frameOnce = describesFrameOnce(fn);
        auto *def = dyn_cast<Defined>(out.sym);
        auto *isec =
            def ? dyn_cast_or_null<InputSection>(def->section) : nullptr;
        uint64_t off = def ? def->value + out.addend : 0;
        if (!frameOnce && isec && off + range <= isec->content().size()) {
          ArrayRef<uint8_t> text = isec->content().slice(off, range);
          for (unsigned k = 0; k < range / 4; ++k)
            if (isCall(read32le(text.data() + 4 * k)))
              fn.calls.push_back(k);
        }
        ok = encodeSync(fn, frameOnce, out.regions);
      } else {
        ok = encodeAsync(fn, out.regions);
      }
      if (!ok)
        interp.why = "region_constraints";
    }
    if (interp.why.empty())
      return;
    out.why = interp.why;
    out.regions.clear();
    if (!interp.escapable) {
      out.dropped = true;
      return;
    }
    if (cie.eh.raReg != 30 ||
        !canonicalEscape(*cie.cfi, *cfi, ctx.arg.isLE, out.escape)) {
      out.why = "non_canonical_cfi";
      out.dropped = true;
    }
  };
  constexpr size_t chunkSize = 256;
  parallelFor(0, divideCeil(refs.size(), chunkSize), [&](size_t c) {
    Scratch scratch;
    for (size_t i = c * chunkSize, e = std::min(refs.size(), i + chunkSize);
         i < e; ++i)
      encodeOne(i, scratch);
  });

  // Assign personality indices, escape offsets and logical descriptors.
  StringMap<uint32_t> escapeOffs;
  funcs.reserve(enc.size());
  for (EncodedFn &e : enc) {
    if (e.dropped && e.why.empty())
      continue;
    if (e.dropped) {
      auto *def = dyn_cast<Defined>(e.sym);
      auto *isec =
          def ? dyn_cast_or_null<InputSectionBase>(def->section) : nullptr;
      if (isec)
        Warn(ctx) << "compact unwind: no unwind info for "
                  << isec->getObjMsg(def->value + e.addend) << " (" << e.why
                  << ")";
      else
        Warn(ctx) << "compact unwind: no unwind info for " << e.sym << " ("
                  << e.why << ")";
      continue;
    }
    Func f{e.sym, e.addend, e.size, 0, e.lsdaSym, e.lsdaAddend, {}};
    const Cie &cie = *e.cie;
    if (cie.pers) {
      auto it = llvm::find_if(pers, [&](const Pers &p) {
        return p.sym == cie.pers && p.addend == cie.persAddend &&
               p.got == cie.persGot;
      });
      if (it == pers.end()) {
        pers.push_back({cie.pers, cie.persAddend, cie.persGot});
        it = pers.end() - 1;
      }
      f.pers = it - pers.begin() + 1;
    }
    // Personality 7 means the personality side table has the index.
    uint64_t persBits = uint64_t(std::min<uint32_t>(f.pers, 7))
                        << a64::PersonalityShift;
    if (!e.escape.empty()) {
      auto [it, inserted] = escapeOffs.try_emplace(e.escape, escapes.size());
      if (inserted)
        escapes += e.escape;
      // Program offset in the high word; flags for CIE augmentations (8
      // signal frame S, 9 MTE-tagged frame G, 10 non-hint PAuth instructions
      // N) and the personality in the low word.
      uint64_t bits = a64::ModeEscape |
                      uint64_t(cie.bKey ? 2 : 0) << a64::RAShift |
                      uint64_t(cie.signal) << a64::EscSignalShift |
                      uint64_t(cie.mte) << a64::EscMTEShift |
                      uint64_t(cie.nonHintPAuth) << a64::EscNonHintShift |
                      persBits | uint64_t(it->second) << a64::EscOffsetShift;
      f.regions.push_back({0, getLDesc(bits, false, false)});
      funcs.push_back(std::move(f));
      continue;
    }
    for (auto &[off, d] : e.regions) {
      uint64_t bits = d.bits | persBits;
      // Windows are relative to the region start, which folding would move, so
      // descriptors with windows don't fold.
      bool pe = d.ps || d.tailLen;
      f.regions.push_back({off * 4, getLDesc(bits, !pe, d.tailLen != 0)});
    }
    funcs.push_back(std::move(f));
  }
  noInfo = getLDesc(a64::ModeNoInfo, true, false);
}

size_t CompactUnwindTable::Impl::layout(uint64_t hdrVA) {
  // A gap between functions needs an entry of its own unless it only holds
  // alignment padding: nothing but NOPs or zeros from the neighbouring input
  // sections, and the linker's fill between sections.
  struct Span {
    uint64_t va, end;
    const InputSection *sec;
  };
  SmallVector<const InputSection *, 0> codeSecs;
  for (OutputSection *os : ctx.outputSections) {
    if (!(os->flags & ELF::SHF_EXECINSTR))
      continue;
    for (SectionCommand *cmd : os->commands)
      if (auto *isd = dyn_cast<InputSectionDescription>(cmd))
        for (const InputSection *isec : isd->sections)
          if (isec->getSize())
            codeSecs.push_back(isec);
  }
  SmallVector<Span, 0> spans(codeSecs.size());

  // The layout depends only on the addresses of the functions, their LSDAs
  // and the code sections, so it needn't be redone if none of them moved.
  size_t n = funcs.size(), m = spans.size();
  SmallVector<uint64_t, 0> vas(2 * n + 2 * m);
  parallelFor(0, n, [&](size_t i) {
    const Func &f = funcs[i];
    vas[i] = f.sym->getVA(ctx) + f.addend;
    vas[n + i] = f.lsdaSym ? f.lsdaSym->getVA(ctx, f.lsdaAddend) : 0;
  });
  parallelFor(0, m, [&](size_t i) {
    const InputSection *isec = codeSecs[i];
    uint64_t va = isec->getVA();
    spans[i] = {va, va + isec->getSize(), isec};
    vas[2 * n + 2 * i] = va;
    vas[2 * n + 2 * i + 1] = va + isec->getSize();
  });
  if (vas == laidOutVAs)
    return totalSize;
  auto byVA = [](const Span &a, const Span &b) { return a.va < b.va; };
  if (!llvm::is_sorted(spans, byVA))
    parallelSort(spans, byVA);

  // Functions by address. Addresses mostly move together, so the last order
  // is usually still sorted.
  auto before = [&](uint32_t a, uint32_t b) {
    return std::make_pair(vas[a], a) < std::make_pair(vas[b], b);
  };
  if (funcOrder.size() != n || !llvm::is_sorted(funcOrder, before)) {
    SmallVector<std::pair<uint64_t, uint32_t>, 0> keyed(n);
    for (size_t i = 0; i < n; ++i)
      keyed[i] = {vas[i], i};
    parallelSort(keyed.begin(), keyed.end());
    funcOrder.resize(n);
    for (size_t i = 0; i < n; ++i)
      funcOrder[i] = keyed[i].second;
  }
  auto isPadding = [&](uint64_t lo, uint64_t hi) {
    auto it = llvm::partition_point(spans,
                                    [&](const Span &s) { return s.end <= lo; });
    for (; it != spans.end() && it->va < hi; ++it) {
      if (isa<SyntheticSection>(it->sec))
        return false;
      ArrayRef<uint8_t> data = it->sec->content();
      for (uint64_t a = std::max(lo, it->va) & ~uint64_t(3),
                    e = std::min(hi, it->end);
           a < e; a += 4) {
        uint64_t off = a - it->va;
        if (off + 4 > data.size())
          return false;
        uint32_t w = read32le(data.data() + off);
        if (w != 0 && w != 0xd503201f)
          return false;
      }
    }
    return true;
  };

  entries.clear();
  lsdaTable.rows.clear();
  persTable.rows.clear();
  uint64_t prevEnd = 0;
  bool havePrev = false, prevEpi = false, prevFoldable = false;
  uint32_t prevDesc = 0;
  auto push = [&](uint64_t va, uint32_t id) {
    const LDesc &l = ldescs[id];
    if (prevFoldable && l.foldable && id == prevDesc && !entries.empty())
      return;
    if (!entries.empty() && entries.back().va == va)
      entries.pop_back();
    entries.push_back({va, id});
    prevDesc = id;
    prevFoldable = l.foldable;
  };
  for (uint32_t i : funcOrder) {
    const Func &f = funcs[i];
    uint64_t va = vas[i];
    if (havePrev && va < prevEnd) {
      // Overlapping FDEs (e.g. duplicated FDEs for one function): keep the
      // first.
      continue;
    }
    // A region with an epilogue can't extend over padding, since its epilogue
    // is anchored at the region's end.
    if (havePrev && va > prevEnd && (prevEpi || !isPadding(prevEnd, va)))
      push(prevEnd, noInfo);
    for (auto [off, id] : f.regions)
      push(va + off, id);
    if (f.pers)
      lsdaTable.rows.push_back({va, i});
    if (f.pers >= 7)
      persTable.rows.push_back({va, i});
    prevEpi = ldescs[f.regions.back().second].hasEpi;
    prevEnd = va + f.size;
    havePrev = true;
  }
  endVA = prevEnd;
  laidOutVAs = std::move(vas);

  // Global descriptors: the most frequent ones.
  SmallVector<uint32_t, 0> counts(ldescs.size());
  for (const Entry &e : entries)
    ++counts[e.ldesc];
  SmallVector<std::pair<int64_t, uint32_t>, 0> keyedCounts;
  for (size_t id = 0; id < counts.size(); ++id)
    if (counts[id])
      keyedCounts.push_back({-int64_t(counts[id]), id});
  llvm::sort(keyedCounts);
  globals.clear();
  for (auto [c, id] : keyedCounts)
    globals.push_back(id);
  if (globals.size() > cu::MaxGlobalDescriptors)
    globals.resize(cu::MaxGlobalDescriptors);
  globalIdx.clear();
  for (size_t i = 0; i < globals.size(); ++i)
    globalIdx[globals[i]] = i;

  // Pages.
  pages.clear();
  size_t ng = globals.size();
  DenseMap<uint32_t, uint32_t> localIdx;
  auto pageBytes = [](size_t count, size_t locals) {
    return alignTo(4 + 4 * count, 8) + 8 * locals;
  };
  for (size_t i = 0; i < entries.size(); ++i) {
    const Entry &e = entries[i];
    bool isGlobal = globalIdx.count(e.ldesc);
    if (!pages.empty()) {
      Page &p = pages.back();
      bool newLocal = !isGlobal && !localIdx.count(e.ldesc);
      size_t nl = p.locals.size() + newLocal;
      if ((e.va - p.va) / 4 < (1u << 20) && nl <= 4096 - ng &&
          pageBytes(p.count + 1, nl) <= cu::PageSize) {
        if (newLocal) {
          localIdx[e.ldesc] = p.locals.size();
          p.locals.push_back(e.ldesc);
        }
        ++p.count;
        continue;
      }
    }
    pages.push_back({e.va, i, 1, {}, 0});
    localIdx.clear();
    if (!isGlobal) {
      localIdx[e.ldesc] = 0;
      pages.back().locals.push_back(e.ldesc);
    }
  }

  // Side table pages: u16 pc offsets, then payloads.
  for (SideTable *t : {&lsdaTable, &persTable}) {
    t->pages.clear();
    size_t w = t->payloadWidth;
    for (size_t i = 0; i < t->rows.size(); ++i) {
      if (!t->pages.empty()) {
        SideTable::Page &p = t->pages.back();
        if ((t->rows[i].va - p.va) / 4 <= 0xffff &&
            alignTo(4 + 2 * (p.count + 1), w) + w * (p.count + 1) <=
                cu::PageSize) {
          ++p.count;
          continue;
        }
      }
      t->pages.push_back({t->rows[i].va, i, 1, 0});
    }
  }

  size_t off = cu::HeaderSize;
  persOff = off;
  off = alignTo(off + 4 * pers.size(), 8);
  dirOff = off;
  off = alignTo(off + cu::SideTableDirEntrySize * sideTables().size(), 8);
  gdescOff = off;
  off += 8 * ng;
  flOff = off;
  off += 8 * (pages.size() + 1);
  for (Page &p : pages) {
    p.off = off;
    off += pageBytes(p.count, p.locals.size());
  }
  sideOff = off;
  for (SideTable *t : sideTables()) {
    t->flOff = off;
    off += 8 * (t->pages.size() + 1);
    for (SideTable::Page &p : t->pages) {
      p.off = alignTo(off, t->payloadWidth);
      off = p.off + alignTo(4 + 2 * p.count, t->payloadWidth) +
            t->payloadWidth * p.count;
    }
    off = alignTo(off, 4);
  }
  escOff = off;
  off += escapes.size();
  totalSize = off;
  return totalSize;
}

void CompactUnwindTable::Impl::write(uint8_t *buf, uint64_t hdrVA) {
  auto rel = [&](uint64_t va) -> uint32_t {
    int64_t v = int64_t(va - hdrVA);
    if (!isInt<32>(v))
      Err(ctx) << "compact unwind: address 0x" << utohexstr(va)
               << " is out of range of .eh_frame_hdr";
    return uint32_t(v);
  };
  buf[cu::HdrVersion] = cu::Version;
  buf[cu::HdrFlags] = kind == CompactUnwindKind::Sync ? cu::FlagSyncOnly : 0;
  write32(ctx, buf + cu::HdrPageCount, pages.size());
  write32(ctx, buf + cu::HdrFirstLevelOffset, flOff);
  write32(ctx, buf + cu::HdrGlobalCount, globals.size());
  write32(ctx, buf + cu::HdrGlobalOffset, gdescOff);
  write32(ctx, buf + cu::HdrPersonalityCount, pers.size());
  write32(ctx, buf + cu::HdrPersonalityOffset, persOff);
  write32(ctx, buf + cu::HdrSideTableCount, sideTables().size());
  write32(ctx, buf + cu::HdrSideTableDirOffset, dirOff);
  write32(ctx, buf + cu::HdrEscapeOffset, escOff);
  write32(ctx, buf + cu::HdrEscapeSize, escapes.size());

  for (size_t i = 0; i < pers.size(); ++i) {
    const Pers &p = pers[i];
    write32(ctx, buf + persOff + 4 * i,
            rel(p.got ? p.sym->getGotVA(ctx) + p.addend
                      : p.sym->getVA(ctx, p.addend)));
  }

  auto finalBits = [&](uint32_t id) { return ldescs[id].bits; };
  for (size_t i = 0; i < globals.size(); ++i)
    write64(ctx, buf + gdescOff + 8 * i, finalBits(globals[i]));

  for (size_t k = 0; k < pages.size(); ++k) {
    const Page &p = pages[k];
    write32(ctx, buf + flOff + 8 * k, rel(p.va));
    write32(ctx, buf + flOff + 8 * k + 4, p.off);
    uint8_t *pb = buf + p.off;
    size_t localOff = p.locals.empty() ? 0 : alignTo(4 + 4 * p.count, 8);
    write16(ctx, pb, p.count);
    write16(ctx, pb + 2, localOff);
    DenseMap<uint32_t, uint32_t> localIdx;
    for (size_t j = 0; j < p.locals.size(); ++j) {
      localIdx[p.locals[j]] = globals.size() + j;
      write64(ctx, pb + localOff + 8 * j, finalBits(p.locals[j]));
    }
    for (size_t j = 0; j < p.count; ++j) {
      const Entry &e = entries[p.first + j];
      auto it = globalIdx.find(e.ldesc);
      uint32_t idx = it != globalIdx.end() ? it->second : localIdx[e.ldesc];
      uint64_t delta = e.va - p.va;
      if (delta % 4)
        Err(ctx) << "compact unwind: misaligned function at 0x"
                 << utohexstr(e.va);
      write32(ctx, pb + 4 + 4 * j,
              uint32_t(delta / 4) << cu::EntryIndexBits | idx);
    }
  }
  write32(ctx, buf + flOff + 8 * pages.size(), rel(endVA));
  write32(ctx, buf + flOff + 8 * pages.size() + 4, 0);

  uint8_t *dir = buf + dirOff;
  for (SideTable *t : sideTables()) {
    dir[0] = t->kind;
    dir[1] = 2;
    dir[2] = t->payloadWidth;
    dir[3] = 0;
    write32(ctx, dir + 4, t->flOff);
    write32(ctx, dir + 8, t->pages.size());
    dir += cu::SideTableDirEntrySize;
    for (size_t k = 0; k < t->pages.size(); ++k) {
      const SideTable::Page &p = t->pages[k];
      write32(ctx, buf + t->flOff + 8 * k, rel(p.va));
      write32(ctx, buf + t->flOff + 8 * k + 4, p.off);
      uint8_t *pb = buf + p.off;
      write16(ctx, pb, p.count);
      write16(ctx, pb + 2, 0);
      uint8_t *pay = pb + alignTo(4 + 2 * p.count, t->payloadWidth);
      for (size_t j = 0; j < p.count; ++j) {
        const SideTable::Row &row = t->rows[p.first + j];
        write16(ctx, pb + 4 + 2 * j, (row.va - p.va) / 4);
        const Func &f = funcs[row.func];
        if (t->kind == cu::SideTableLSDA)
          write32(ctx, pay + 4 * j,
                  f.lsdaSym ? rel(f.lsdaSym->getVA(ctx, f.lsdaAddend)) : 0);
        else
          write16(ctx, pay + 2 * j, f.pers - 1);
      }
    }
    write32(ctx, buf + t->flOff + 8 * t->pages.size(), rel(endVA));
    write32(ctx, buf + t->flOff + 8 * t->pages.size() + 4, 0);
  }

  memcpy(buf + escOff, escapes.data(), escapes.size());
}

void elf::requestCompactUnwindGotEntries(Ctx &ctx) {
  for (EhInputSection *sec : ctx.in.ehFrame->sections) {
    for (const EhSectionPiece &piece : sec->cies) {
      EhCie cie = parseCie(*sec, piece);
      if (cie.persEnc == DW_EH_PE_omit || (cie.persEnc & DW_EH_PE_indirect))
        continue;
      unsigned k = piece.firstRelocation;
      if (k == unsigned(-1) ||
          sec->rels[k].offset != piece.inputOff + cie.persOff)
        continue;
      const Relocation &r = sec->rels[k];
      if (r.addend)
        Err(ctx) << sec->getObjMsg(r.offset)
                 << ": -z compact-unwind needs a symbol for the personality "
                    "routine, not a symbol plus an addend";
      else
        r.sym->setFlags(NEEDS_GOT);
    }
  }
}

CompactUnwindTable::CompactUnwindTable(Ctx &ctx)
    : impl(std::make_unique<Impl>(ctx)) {}
CompactUnwindTable::~CompactUnwindTable() = default;
void CompactUnwindTable::encode() { impl->encode(); }
size_t CompactUnwindTable::layout(uint64_t hdrVA) {
  return impl->layout(hdrVA);
}
void CompactUnwindTable::write(uint8_t *buf, uint64_t hdrVA) {
  impl->write(buf, hdrVA);
}
