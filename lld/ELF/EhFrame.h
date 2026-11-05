//===- EhFrame.h ------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_EHFRAME_H
#define LLD_ELF_EHFRAME_H

#include "lld/Common/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/Dwarf.h"

namespace lld::elf {
class EhInputSection;
class InputSection;
class Symbol;
struct EhSectionPiece;

struct EhCie {
  StringRef aug;
  uint64_t codeAlign = 0, raReg = 0;
  int64_t dataAlign = 0;
  uint8_t fdeEnc = llvm::dwarf::DW_EH_PE_absptr;
  uint8_t lsdaEnc = llvm::dwarf::DW_EH_PE_omit;
  uint8_t persEnc = llvm::dwarf::DW_EH_PE_omit;
  // Offset of the personality field from the start of the CIE.
  uint64_t persOff = 0;
  ArrayRef<uint8_t> insns;
};

struct EhFde {
  uint64_t pcRange = 0;
  // Offset of the LSDA field from the start of the FDE, and its raw value.
  uint64_t lsdaOff = 0, lsda = 0;
  ArrayRef<uint8_t> insns;
};

EhCie parseCie(const EhInputSection &sec, const EhSectionPiece &p);
EhFde parseFde(const EhInputSection &sec, const EhSectionPiece &p,
               const EhCie &cie);
uint8_t getFdeEncoding(const EhInputSection &sec, const EhSectionPiece &p);
bool hasLSDA(const EhInputSection &sec, const EhSectionPiece &p);
bool hasZeroPcRange(const EhInputSection &sec, const EhSectionPiece &p,
                    uint8_t enc);

// An FDE's function and LSDA, and the personality routine, if any, that
// interprets the LSDA.
struct FdeLsda {
  InputSection *fn;
  InputSection *lsda;
  uint64_t lsdaOff;
  Symbol *pers;
  int64_t persAddend;
};

// Calls fn for each FDE in sec whose function and LSDA are in input sections.
void forEachFdeLsda(const EhInputSection &sec,
                    llvm::function_ref<void(const FdeLsda &)> fn);
}

#endif
