//===- EhFrame.cpp -------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// .eh_frame section contains information on how to unwind the stack when
// an exception is thrown. The section consists of sequence of CIE and FDE
// records. The linker needs to merge CIEs and associate FDEs to CIEs.
// That means the linker has to understand the format of the section.
//
// This file contains a few utility functions to read .eh_frame contents.
//
//===----------------------------------------------------------------------===//

#include "EhFrame.h"
#include "Config.h"
#include "InputFiles.h"
#include "InputSection.h"
#include "Relocations.h"
#include "Symbols.h"
#include "Target.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/Support/LEB128.h"
#include <optional>

using namespace llvm;
using namespace llvm::ELF;
using namespace llvm::dwarf;
using namespace llvm::object;
using namespace lld;
using namespace lld::elf;

namespace {
class EhReader {
public:
  EhReader(const InputSectionBase *s, ArrayRef<uint8_t> d) : isec(s), d(d) {}
  EhCie readCie();
  EhFde readFde(const EhCie &cie);

private:
  template <class P> void errOn(const P *loc, const Twine &msg) {
    Ctx &ctx = isec->file->ctx;
    Err(ctx) << "corrupted .eh_frame: " << msg << "\n>>> defined in "
             << isec->getObjMsg((const uint8_t *)loc - isec->content().data());
  }

  uint8_t readByte();
  void skipBytes(size_t count);
  StringRef readString();
  uint64_t readULEB128();
  int64_t readSLEB128();
  uint64_t readFixed(size_t size);
  uint8_t skipAugP();

  const InputSectionBase *isec;
  ArrayRef<uint8_t> d;
};
}

// Read a byte and advance D by one byte.
uint8_t EhReader::readByte() {
  if (d.empty()) {
    errOn(d.data(), "unexpected end of CIE");
    return 0;
  }
  uint8_t b = d.front();
  d = d.slice(1);
  return b;
}

void EhReader::skipBytes(size_t count) {
  if (d.size() < count)
    errOn(d.data(), "CIE is too small");
  else
    d = d.slice(count);
}

// Read a null-terminated string.
StringRef EhReader::readString() {
  const uint8_t *end = llvm::find(d, '\0');
  if (end == d.end()) {
    errOn(d.data(), "corrupted CIE (failed to read string)");
    return {};
  }
  StringRef s = toStringRef(d.slice(0, end - d.begin()));
  d = d.slice(s.size() + 1);
  return s;
}

uint64_t EhReader::readULEB128() {
  unsigned n;
  const char *err = nullptr;
  uint64_t v = decodeULEB128(d.data(), &n, d.end(), &err);
  if (err) {
    errOn(d.data(), "corrupted CIE (failed to read LEB128)");
    d = {};
    return 0;
  }
  d = d.slice(n);
  return v;
}

int64_t EhReader::readSLEB128() {
  unsigned n;
  const char *err = nullptr;
  int64_t v = decodeSLEB128(d.data(), &n, d.end(), &err);
  if (err) {
    errOn(d.data(), "corrupted CIE (failed to read LEB128)");
    d = {};
    return 0;
  }
  d = d.slice(n);
  return v;
}

// Read an unsigned integer of the given size in the target's byte order.
uint64_t EhReader::readFixed(size_t size) {
  if (d.size() < size) {
    errOn(d.data(), "corrupted FDE");
    d = {};
    return 0;
  }
  bool isLE = isec->file->ctx.arg.isLE;
  uint64_t v = 0;
  for (size_t i = 0; i < size; ++i)
    v |= uint64_t(d[i]) << (8 * (isLE ? i : size - 1 - i));
  d = d.slice(size);
  return v;
}

static size_t getAugPSize(Ctx &ctx, unsigned enc) {
  switch (enc & 0x0f) {
  case DW_EH_PE_absptr:
  case DW_EH_PE_signed:
    return ctx.arg.wordsize;
  case DW_EH_PE_udata2:
  case DW_EH_PE_sdata2:
    return 2;
  case DW_EH_PE_udata4:
  case DW_EH_PE_sdata4:
    return 4;
  case DW_EH_PE_udata8:
  case DW_EH_PE_sdata8:
    return 8;
  }
  return 0;
}

// Skip a pointer-encoding byte and the pointer it describes, and return the
// encoding.
uint8_t EhReader::skipAugP() {
  uint8_t enc = readByte();
  if ((enc & 0xf0) == DW_EH_PE_aligned) {
    errOn(d.data() - 1, "DW_EH_PE_aligned encoding is not supported");
    return enc;
  }
  size_t size = getAugPSize(isec->getCtx(), enc);
  if (size == 0)
    errOn(d.data() - 1, "unknown FDE encoding");
  else if (size >= d.size())
    errOn(d.data() - 1, "corrupted CIE");
  else
    d = d.slice(size);
  return enc;
}

EhCie elf::parseCie(const EhInputSection &sec, const EhSectionPiece &p) {
  return EhReader(&sec, p.data(sec)).readCie();
}

EhFde elf::parseFde(const EhInputSection &sec, const EhSectionPiece &p,
                    const EhCie &cie) {
  return EhReader(&sec, p.data(sec)).readFde(cie);
}

uint8_t elf::getFdeEncoding(const EhInputSection &sec,
                            const EhSectionPiece &p) {
  return parseCie(sec, p).fdeEnc;
}

bool elf::hasLSDA(const EhInputSection &sec, const EhSectionPiece &p) {
  return parseCie(sec, p).aug.contains('L');
}

// pc_range follows the 4-byte length, 4-byte CIE pointer, and pc_begin, and
// uses pc_begin's encoding.
bool elf::hasZeroPcRange(const EhInputSection &sec, const EhSectionPiece &p,
                         uint8_t enc) {
  Ctx &ctx = sec.getCtx();
  size_t pSize = getAugPSize(ctx, enc);
  ArrayRef<uint8_t> d = p.data(sec);
  if (pSize == 0 || d.size() < 8 + 2 * pSize ||
      !llvm::all_of(d.slice(8 + pSize, pSize), [](uint8_t c) { return !c; }))
    return false;
  // With linker relaxation, pc_range is computed by an ADD/SUB relocation pair,
  // which GNU as emits even for an empty function. The range is zero if both
  // relocations resolve to the same address.
  if (ctx.arg.emachine == EM_LOONGARCH || ctx.arg.emachine == EM_RISCV) {
    uint64_t off = p.inputOff + 8 + pSize;
    SmallVector<uint64_t, 2> vals;
    for (size_t i = p.firstRelocation;
         i < sec.rels.size() && sec.rels[i].offset < off + pSize; ++i)
      if (const Relocation &r = sec.rels[i]; r.offset >= off)
        vals.push_back(r.sym->getVA(ctx, r.addend));
    return vals.empty() || (vals.size() == 2 && vals[0] == vals[1]);
  }
  return true;
}

EhCie EhReader::readCie() {
  EhCie c;
  const uint8_t *start = d.data();
  skipBytes(8);
  int version = readByte();
  if (version != 1 && version != 3) {
    errOn(d.data() - 1,
          "FDE version 1 or 3 expected, but got " + Twine(version));
    return c;
  }

  c.aug = readString();
  c.codeAlign = readULEB128();
  c.dataAlign = readSLEB128();
  // In CIE version 1 the return address register is a single byte. In CIE
  // version 3 it is an unsigned LEB128.
  c.raReg = version == 1 ? readByte() : readULEB128();

  // Augmentation data is not in TLV (type-length-value) format, so each
  // record type has to be known to find the ones after it.
  std::optional<ArrayRef<uint8_t>> afterAug;
  for (char ch : c.aug) {
    switch (ch) {
    case 'z': {
      uint64_t len = readULEB128();
      if (len > d.size()) {
        errOn(d.data(), "corrupted CIE");
        return c;
      }
      afterAug = d.slice(len);
      break;
    }
    case 'L':
      c.lsdaEnc = readByte();
      break;
    case 'R':
      c.fdeEnc = readByte();
      break;
    case 'P':
      c.persOff = d.data() + 1 - start;
      c.persEnc = skipAugP();
      break;
    case 'B':
    case 'S':
    case 'G':
    case 'N':
      break;
    default:
      errOn(c.aug.data(), "unknown .eh_frame augmentation string: " + c.aug);
      return c;
    }
  }
  c.insns = afterAug ? *afterAug : d;
  return c;
}

// pc_begin and pc_range follow the 4-byte length and 4-byte CIE pointer, in
// the CIE's FDE encoding. With a 'z' augmentation, augmentation data (of which
// the linker reads only the LSDA pointer) precedes the instructions.
EhFde EhReader::readFde(const EhCie &cie) {
  EhFde f;
  const uint8_t *start = d.data();
  Ctx &ctx = isec->getCtx();
  size_t pSize = getAugPSize(ctx, cie.fdeEnc);
  skipBytes(8 + pSize);
  f.pcRange = readFixed(pSize);
  if (cie.aug.starts_with("z")) {
    uint64_t len = readULEB128();
    if (len > d.size()) {
      errOn(d.data(), "corrupted FDE");
      return f;
    }
    ArrayRef<uint8_t> insns = d.slice(len);
    if (cie.lsdaEnc != DW_EH_PE_omit) {
      f.lsdaOff = d.data() - start;
      f.lsda = readFixed(getAugPSize(ctx, cie.lsdaEnc));
    }
    d = insns;
  }
  f.insns = d;
  return f;
}
