//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//
//  Lookup and decoding of AArch64 compact unwind tables, which replace
//  .eh_frame and are stored in .eh_frame_hdr (see
//  llvm/docs/AArch64ELFCompactUnwind.md).
//
//===----------------------------------------------------------------------===//

#ifndef __ELFCOMPACTUNWIND_HPP__
#define __ELFCOMPACTUNWIND_HPP__

#include "AArch64CompactUnwind.hpp"
#include "DwarfParser.hpp"
#include "config.h"
#include "libunwind.h"

#if defined(_LIBUNWIND_SUPPORT_ELF_COMPACT_UNWIND)

namespace libunwind {

namespace ecu = aarch64_compact_unwind;

template <typename A> class ElfCompactUnwind {
public:
  typedef typename A::pint_t pint_t;
  typedef typename CFI_Parser<A>::PrologInfo PrologInfo;
  typedef typename CFI_Parser<A>::CIE_Info CIE_Info;
  typedef typename CFI_Parser<A>::FDE_Info FDE_Info;

  // unw_proc_info_t::format of frames described by a compact descriptor.
  static const uint32_t kFormat = _LIBUNWIND_ELF_COMPACT_FORMAT;

  enum { kVersion = 3 };

  struct Info {
    uint64_t desc;
    // PARTIAL: the layout descriptor; escape: the program address; otherwise
    // the region length in instructions.
    uint64_t aux;
    pint_t regionStart;
    pint_t regionEnd;
    pint_t funcStart;
    pint_t lsda;
    pint_t personality;
    // The PAuth_LR signing instruction, for RA schemes that sign with the PC.
    pint_t pacSite;
  };

  static bool isCompact(A &addressSpace, pint_t hdr) {
    return addressSpace.get8(hdr) == kVersion;
  }

  // Returns false if pc is not covered by the table.
  static bool findInfo(A &addressSpace, pint_t hdr, pint_t pc, Info &info);

  // Computes the unwind rules at pcOffset bytes into a region. For escape
  // programs the region starts at the function start.
  template <typename R>
  static bool decode(A &addressSpace, uint64_t desc, uint64_t aux,
                     pint_t regionStart, pint_t pcOffset, pint_t pacSite,
                     PrologInfo *prolog, CIE_Info *cieInfo);

  static bool signsWithPC(uint64_t desc) {
    unsigned mode = desc & 7;
    return mode != ecu::kModeEscape && ((desc >> ecu::kRaShift) & 7) >= 3;
  }

private:
  static pint_t sectionRel(A &addressSpace, pint_t hdr, pint_t field) {
    return hdr + (pint_t)(int64_t)(int32_t)addressSpace.get32(field);
  }
  static bool findSideRow(A &addressSpace, pint_t hdr, unsigned kind, pint_t pc,
                          pint_t &rowStart, uint32_t &payload);
  static int dwarfReg(int csr) {
    return csr < 10           ? 19 + csr
           : csr == ecu::kX29 ? 29
           : csr == ecu::kX30 ? 30
                              : 72 + csr - 12;
  }
};

template <typename A>
bool ElfCompactUnwind<A>::findInfo(A &addressSpace, pint_t hdr, pint_t pc,
                                   Info &info) {
  uint32_t pageCount = addressSpace.get32(hdr + 4);
  pint_t firstLevel = hdr + addressSpace.get32(hdr + 8);
  uint32_t globalCount = addressSpace.get32(hdr + 12);
  pint_t globals = hdr + addressSpace.get32(hdr + 16);
  if (pageCount == 0 || pc < sectionRel(addressSpace, hdr, firstLevel) ||
      pc >= sectionRel(addressSpace, hdr, firstLevel + 8 * pageCount))
    return false;
  uint32_t lo = 0, hi = pageCount;
  while (hi - lo > 1) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (sectionRel(addressSpace, hdr, firstLevel + 8 * mid) <= pc)
      lo = mid;
    else
      hi = mid;
  }
  pint_t pagePC = sectionRel(addressSpace, hdr, firstLevel + 8 * lo);
  pint_t page = hdr + addressSpace.get32(firstLevel + 8 * lo + 4);
  uint32_t count = addressSpace.get16(page);
  uint32_t localOff = addressSpace.get16(page + 2);
  pint_t entries = page + 4;
  uint32_t delta = (uint32_t)((pc - pagePC) / 4);
  uint32_t l = 0, h = count;
  while (h - l > 1) {
    uint32_t mid = l + (h - l) / 2;
    if ((addressSpace.get32(entries + 4 * mid) >> 12) <= delta)
      l = mid;
    else
      h = mid;
  }
  uint32_t entry = addressSpace.get32(entries + 4 * l);
  info.regionStart = pagePC + 4 * (pint_t)(entry >> 12);
  if (l + 1 < count)
    info.regionEnd =
        pagePC + 4 * (pint_t)(addressSpace.get32(entries + 4 * (l + 1)) >> 12);
  else
    info.regionEnd = sectionRel(addressSpace, hdr, firstLevel + 8 * (lo + 1));

  auto descAt = [&](uint32_t idx) -> uint64_t {
    if (idx < globalCount)
      return addressSpace.get64(globals + 8 * idx);
    return addressSpace.get64(page + localOff + 8 * (idx - globalCount));
  };
  info.desc = descAt(entry & 0xfff);
  info.lsda = 0;
  info.personality = 0;
  info.pacSite = 0;
  info.funcStart = info.regionStart;
  unsigned mode = info.desc & 7;
  if (mode >= ecu::kModeNoInfo)
    return true;
  bool partial = mode == ecu::kModePartialFp || mode == ecu::kModePartialSp;
  if (partial) {
    uint32_t layout = (info.desc >> ecu::kLayoutShift) & 0xfff;
    // Any page can use a global descriptor, so its layout must be global.
    if ((entry & 0xfff) < globalCount && layout >= globalCount)
      return false;
    info.aux = descAt(layout);
  } else if (mode == ecu::kModeEscape)
    info.aux = hdr + addressSpace.get32(hdr + 36) +
               (pint_t)(info.desc >> ecu::kEscOffsetShift);
  else
    info.aux = (info.regionEnd - info.regionStart) / 4;

  if (signsWithPC(info.desc)) {
    // The signing instruction is at the end of a prologue window's lead-in;
    // or in N, region-relative, where nothing else uses N (4095: not
    // representable); or in the side table.
    bool hasPro = !partial && ((info.desc >> ecu::kHasProShift) & 1);
    bool hasEpi = !partial && ((info.desc >> ecu::kHasEpiShift) & 1);
    unsigned apx = (info.desc >> ecu::kAposShift) & 3;
    unsigned apos = apx ? apx - 1 : 2;
    unsigned conv = ((partial ? info.aux : info.desc) >> ecu::kConvShift) & 3;
    unsigned n = (info.desc >> ecu::kNShift) & 0xfff;
    // GCC's sp-anchored convention (2) uses N for its slots.
    bool nFree =
        !hasPro && !hasEpi &&
        (mode == ecu::kModeNull ||
         ((mode == ecu::kModeFp || mode == ecu::kModePartialFp) && conv != 2));
    if (hasPro && apos >= 1) {
      info.pacSite = info.regionStart + 4 * (pint_t)(apos - 1);
    } else if (nFree && n != 4095) {
      info.pacSite = info.regionStart + 4 * (pint_t)n;
    } else {
      pint_t rowStart;
      uint32_t site;
      if (!findSideRow(addressSpace, hdr, 0x80, pc, rowStart, site))
        return false;
      info.pacSite = rowStart + 4 * (pint_t)(int64_t)(int16_t)site;
    }
  }

  unsigned pers = (info.desc >> ecu::kPersShift) & 7;
  if (pers == 0)
    return true;
  // Every function with a personality routine has an LSDA row, so the row
  // found for pc is at the function's start.
  uint32_t lsda;
  if (!findSideRow(addressSpace, hdr, 1, pc, info.funcStart, lsda))
    return false;
  info.lsda = lsda ? hdr + (pint_t)(int64_t)(int32_t)lsda : 0;
  uint32_t persIndex = pers - 1;
  if (pers == 7) {
    pint_t rowStart;
    if (!findSideRow(addressSpace, hdr, 2, pc, rowStart, persIndex) ||
        rowStart != info.funcStart)
      return false;
  }
  if (persIndex >= addressSpace.get32(hdr + 20))
    return false;
  int32_t persEntry = (int32_t)addressSpace.get32(
      hdr + addressSpace.get32(hdr + 24) + 4 * persIndex);
  info.personality =
      (pint_t)addressSpace.getP(hdr + (pint_t)(int64_t)persEntry);
  return true;
}

// Finds the last row at or before pc in the side table of the given kind.
template <typename A>
bool ElfCompactUnwind<A>::findSideRow(A &addressSpace, pint_t hdr,
                                      unsigned kind, pint_t pc,
                                      pint_t &rowStart, uint32_t &payload) {
  uint32_t tables = addressSpace.get32(hdr + 28);
  pint_t dir = hdr + addressSpace.get32(hdr + 32);
  for (uint32_t t = 0; t < tables; ++t, dir += 12) {
    if (addressSpace.get8(dir) != kind)
      continue;
    unsigned pcWidth = addressSpace.get8(dir + 1);
    unsigned payWidth = addressSpace.get8(dir + 2);
    pint_t fl = hdr + addressSpace.get32(dir + 4);
    uint32_t pages = addressSpace.get32(dir + 8);
    if (pages == 0 || pc < sectionRel(addressSpace, hdr, fl) ||
        (pcWidth != 2 && pcWidth != 4) || (payWidth != 2 && payWidth != 4))
      return false;
    uint32_t lo = 0, hi = pages;
    while (hi - lo > 1) {
      uint32_t mid = lo + (hi - lo) / 2;
      if (sectionRel(addressSpace, hdr, fl + 8 * mid) <= pc)
        lo = mid;
      else
        hi = mid;
    }
    pint_t pagePC = sectionRel(addressSpace, hdr, fl + 8 * lo);
    pint_t page = hdr + addressSpace.get32(fl + 8 * lo + 4);
    uint32_t n = addressSpace.get16(page);
    // Flag 1 (LSDA tables): an s32 base LSDA follows, and payloads are u16
    // offsets from it in 4-byte units (0xffff: none).
    bool compressed = addressSpace.get16(page + 2) & 1;
    pint_t pcs = page + (compressed ? 8 : 4);
    auto rowOff = [&](uint32_t k) -> uint32_t {
      return pcWidth == 2 ? addressSpace.get16(pcs + 2 * k)
                          : addressSpace.get32(pcs + 4 * k);
    };
    uint32_t delta = (uint32_t)((pc - pagePC) / 4);
    uint32_t rl = 0, rh = n;
    while (rh - rl > 1) {
      uint32_t mid = rl + (rh - rl) / 2;
      if (rowOff(mid) <= delta)
        rl = mid;
      else
        rh = mid;
    }
    rowStart = pagePC + 4 * (pint_t)rowOff(rl);
    if (compressed) {
      if (kind != 1 || pcWidth != 2)
        return false;
      uint32_t off = addressSpace.get16(pcs + 2 * n + 2 * rl);
      payload = off == 0xffff ? 0 : addressSpace.get32(page + 4) + 4 * off;
      return true;
    }
    pint_t payloads =
        page + ((4 + pcWidth * n + payWidth - 1) & ~(pint_t)(payWidth - 1));
    payload = payWidth == 2 ? addressSpace.get16(payloads + 2 * rl)
                            : addressSpace.get32(payloads + 4 * rl);
    return true;
  }
  return false;
}

template <typename A>
template <typename R>
bool ElfCompactUnwind<A>::decode(A &addressSpace, uint64_t desc, uint64_t aux,
                                 pint_t regionStart, pint_t pcOffset,
                                 pint_t pacSite, PrologInfo *prolog,
                                 CIE_Info *cieInfo) {
  unsigned mode = desc & 7;
  unsigned raField = (desc >> ecu::kRaShift) & 7;
  memset(cieInfo, 0, sizeof(*cieInfo));
  cieInfo->returnAddressRegister = 30;
  cieInfo->codeAlignFactor = 4;
  cieInfo->dataAlignFactor = -8;
#if defined(_LIBUNWIND_TARGET_AARCH64)
  // 1/2: A/B key; 3/4: A/B with the PC (PACM hint); 5/6: A/B with the PC
  // (non-hint instructions).
  cieInfo->addressesSignedWithBKey =
      raField == 2 || raField == 4 || raField == 6;
  cieInfo->nonHintPAuthFrame = raField >= 5;
#endif

  if (mode == ecu::kModeEscape) {
    // A ULEB length and DWARF CFA instructions relative to the implied
    // initial state: code alignment 4, data alignment -8, return address in
    // x30, CFA = sp, nothing saved. Bits 8-10 carry the CIE augmentations
    // S, G and N.
    pint_t p = (pint_t)aux;
    pint_t len = (pint_t)addressSpace.getULEB128(p, (pint_t)-1);
    cieInfo->cieStart = p;
    cieInfo->cieInstructions = p;
    cieInfo->cieLength = 0;
    cieInfo->isSignalFrame = (desc >> ecu::kEscSignalShift) & 1;
#if defined(_LIBUNWIND_TARGET_AARCH64)
    cieInfo->mteTaggedFrame = (desc >> ecu::kEscMteShift) & 1;
    cieInfo->nonHintPAuthFrame = (desc >> ecu::kEscNonHintShift) & 1;
#endif
    FDE_Info fdeInfo;
    fdeInfo.fdeStart = p;
    fdeInfo.fdeInstructions = p;
    fdeInfo.fdeLength = len;
    fdeInfo.pcStart = regionStart;
    fdeInfo.pcEnd = (pint_t)-1;
    fdeInfo.lsda = 0;
    prolog->cfaRegister = 31;
    prolog->cfaRegisterOffset = 0;
    return CFI_Parser<A>::template parseFDEInstructions<R>(
        addressSpace, fdeInfo, *cieInfo, regionStart + pcOffset + 1,
        R::getArch(), prolog);
  }
  ecu::Shape shape;
  if (!ecu::decodeShape(desc, aux, shape))
    return false;
  // aux is the region length, except for PARTIAL descriptors, which have no
  // prologue or epilogue.
  ecu::Frame f = ecu::frameAt(shape, shape.partial ? 0 : (unsigned)aux,
                              (unsigned)(pcOffset / 4));
  prolog->cfaRegister = f.cfaFp ? 29 : 31;
  prolog->cfaRegisterOffset = f.cfaOff;
  for (int r = 0; r < ecu::kNumCsr; ++r) {
    if (!(f.saved >> r & 1))
      continue;
    prolog->savedRegisters[dwarfReg(r)].location =
        CFI_Parser<A>::kRegisterInCFA;
    prolog->savedRegisters[dwarfReg(r)].value = shape.slots.off[r];
  }
  if (f.x18) {
    // DW_CFA_val_expression x18, DW_OP_breg18 -8
    static const uint8_t scsExpr[] = {2, 0x82, 0x78};
    prolog->savedRegisters[18].location = CFI_Parser<A>::kRegisterIsExpression;
    prolog->savedRegisters[18].value = (int64_t)(uintptr_t)scsExpr;
  }
#if defined(_LIBUNWIND_TARGET_AARCH64)
  // Like DW_CFA_AARCH64_negate_ra_state, which makes the sign state a pseudo
  // register that the step copies into the caller's registers.
  if (shape.ra) {
    prolog->savedRegisters[UNW_AARCH64_RA_SIGN_STATE].location =
        CFI_Parser<A>::kRegisterIsPseudo;
    prolog->savedRegisters[UNW_AARCH64_RA_SIGN_STATE].value =
        f.raSigned ? (shape.withPC ? 2 : 1) : 0;
    if (shape.withPC)
      prolog->ptrAuthDiversifier = pacSite;
  }
#endif
  return true;
}

} // namespace libunwind

#endif // defined(_LIBUNWIND_SUPPORT_ELF_COMPACT_UNWIND)

#endif // __ELFCOMPACTUNWIND_HPP__
