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
// llvm/docs/AArch64ELFCompactUnwind.md). Each live FDE's CFI is rewritten as an
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
#include "llvm/Support/Endian.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/Parallel.h"
#include <optional>

using namespace llvm;
using namespace llvm::dwarf;
using namespace llvm::support::endian;
using namespace lld;
using namespace lld::elf;

namespace cu = llvm::ELFCompactUnwind;
namespace a64 = llvm::ELFCompactUnwind::AArch64;

namespace {
constexpr unsigned kSP = 31;

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
  StringRef why; // why the function was dropped
  bool dropped = false;
  Symbol *sym = nullptr;
  int64_t addend = 0;
  uint32_t size = 0;
  const Cie *cie = nullptr;
  Symbol *lsdaSym = nullptr;
  int64_t lsdaAddend = 0;
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

  uint32_t getLDesc(uint64_t bits, bool foldable) {
    auto [it, inserted] = ldescIds.try_emplace(bits, ldescs.size());
    if (inserted)
      ldescs.push_back({bits, foldable});
    return it->second;
  }

  void encode();
  size_t layout(uint64_t hdrVA);
  void write(uint8_t *buf, uint64_t hdrVA);
};

void CompactUnwindTable::Impl::encode() {
  EhFrameSection &ehFrame = *ctx.in.ehFrame;

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

  // Functions are encoded in chunks, each reusing a CFI program.
  struct Scratch {
    std::optional<dwarf::CFIProgram> cfi;
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
    f.regions.push_back({0, getLDesc(bits, false)});
    funcs.push_back(std::move(f));
  }
  noInfo = getLDesc(a64::ModeNoInfo, true);
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
  bool havePrev = false, prevFoldable = false;
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
    if (havePrev && va > prevEnd && !isPadding(prevEnd, va))
      push(prevEnd, noInfo);
    for (auto [off, id] : f.regions)
      push(va + off, id);
    if (f.pers)
      lsdaTable.rows.push_back({va, i});
    if (f.pers >= 7)
      persTable.rows.push_back({va, i});
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
